#include "output.h"
#include "buffer_pool.h"
#include "cso.h"

namespace maxcso {

static const size_t QUEUE_SIZE = 128;

Output::Output(uv_loop_t *loop) : loop_(loop), srcSize_(-1), index_(nullptr), writing_(false) {
	for (size_t i = 0; i < QUEUE_SIZE; ++i) {
		freeSectors_.push_back(new Sector());
	}
}

Output::~Output() {
	for (Sector *sector : freeSectors_) {
		delete sector;
	}
	for (auto pair : pendingSectors_) {
		delete pair.second;
	}
	freeSectors_.clear();
	pendingSectors_.clear();

	delete [] index_;
	index_ = nullptr;
}

void Output::SetFile(uv_file file, int64_t srcSize) {
	file_ = file;
	srcSize_ = srcSize;
	srcPos_ = 0;

	const uint32_t sectors = static_cast<uint32_t>(srcSize >> SECTOR_SHIFT);
	// Start after the header and index, which we'll fill in later.
	index_ = new uint32_t[sectors + 1];
	// Start after the end of the index data and header.
	dstPos_ = sizeof(CSOHeader) + (sectors + 1) * sizeof(uint32_t);

	// TODO: We might be able to optimize shift better by running through the data.
	// That would require either a second pass or keeping the entire result in RAM.
	// For now, just take worst case (all blocks stored uncompressed.)
	int64_t worstSize = dstPos_ + srcSize;
	shift_ = 0;
	for (int i = 62; i >= 31; --i) {
		int64_t max = 1LL << i;
		if (worstSize >= max) {
			// This means we need i + 1 bits to store the position.
			// We have to shift enough off to fit into 31.
			shift_ = i + 1 - 31;
			break;
		}
	}

	// If the shift is above 11, the padding could make it need more space.
	// But that would be > 4 TB anyway, so let's not worry about it.
	align_ = 1 << shift_;
	Align(dstPos_);
}

int32_t Output::Align(int64_t &pos) {
	uint32_t off = static_cast<uint32_t>(pos % align_);
	if (off != 0) {
		pos += align_ - off;
		return align_ - off;
	}
	return 0;
}

void Output::Enqueue(int64_t pos, uint8_t *buffer) {
	Sector *sector = freeSectors_.back();
	freeSectors_.pop_back();

	// TODO: Don't compress certain blocks?  Like header, dirs?
	bool tryCompress = true;

	// Sector takes ownership of buffer.
	if (tryCompress) {
		sector->Process(loop_, pos, buffer, [this, sector](bool status, const char *reason) {
			HandleReadySector(sector);
		});
	} else {
		sector->Reserve(pos, buffer);
		HandleReadySector(sector);
	}
}

void Output::HandleReadySector(Sector *sector) {
	if (sector != nullptr) {
		if (srcPos_ != sector->Pos()) {
			// We're not there yet in the file stream.  Queue this, get to it later.
			pendingSectors_[sector->Pos()] = sector;
			return;
		}
	} else {
		// If no sector was provided, we're looking at the first in the queue.
		if (pendingSectors_.empty()) {
			return;
		}
		sector = pendingSectors_.begin()->second;
		if (srcPos_ != sector->Pos()) {
			return;
		}

		// Remove it from the queue, and then run with it.
		pendingSectors_.erase(pendingSectors_.begin());
	}

	// Check for any sectors that immediately follow the one we're writing.
	// We'll just write them all together.
	std::vector<Sector *> sectors;
	sectors.push_back(sector);
	// TODO: Try other numbers.
	static const size_t MAX_BUFS = 4;
	int64_t nextPos = srcPos_ + SECTOR_SIZE;
	auto it = pendingSectors_.find(nextPos);
	while (it != pendingSectors_.end()) {
		sectors.push_back(it->second);
		pendingSectors_.erase(it);
		nextPos += SECTOR_SIZE;
		it = pendingSectors_.find(nextPos);

		// Don't do more than 4 at a time.
		if (sectors.size() >= MAX_BUFS) {
			break;
		}
	}

	int64_t dstPos = dstPos_;
	uv_buf_t bufs[MAX_BUFS * 2];
	unsigned int nbufs = 0;
	static char padding[2048] = {0};
	for (size_t i = 0; i < sectors.size(); ++i) {
		bufs[nbufs++] = uv_buf_init(reinterpret_cast<char *>(sectors[i]->BestBuffer()), sectors[i]->BestSize());

		// Update the index.
		const int32_t s = static_cast<int32_t>(sectors[i]->Pos() >> SECTOR_SHIFT);
		index_[s] = static_cast<int32_t>(dstPos >> shift_);
		if (!sectors[i]->Compressed()) {
			index_[s] |= CSO_INDEX_UNCOMPRESSED;
		}

		dstPos += sectors[i]->BestSize();
		int32_t padSize = Align(dstPos);
		if (padSize != 0) {
			// We need uv to write the padding out as well.
			bufs[nbufs++] = uv_buf_init(padding, padSize);
		}
	}

	const int64_t totalWrite = dstPos - dstPos_;
	uv_.fs_write(loop_, sector->WriteReq(), file_, bufs, nbufs, dstPos_, [this, sectors, nextPos, totalWrite](uv_fs_t *req) {
		for (Sector *sector : sectors) {
			sector->Release();
			freeSectors_.push_back(sector);
		}

		if (req->result != totalWrite) {
			finish_(false, "Data could not be written to output file");
			uv_fs_req_cleanup(req);
			return;
		}
		uv_fs_req_cleanup(req);

		srcPos_ = nextPos;
		dstPos_ += totalWrite;

		// TODO: Better (including index?)
		float prog = static_cast<float>(srcPos_) / static_cast<float>(srcSize_);
		progress_(prog);

		// Check if there's more data to write out.
		HandleReadySector(nullptr);
	});
}

bool Output::QueueFull() {
	return freeSectors_.empty();
}

void Output::OnProgress(OutputCallback callback) {
	progress_ = callback;
}

void Output::OnFinish(OutputFinishCallback callback) {
	finish_ = callback;
}

void Output::Flush() {
	CSOHeader *header = new CSOHeader;
	memcpy(header->magic, CSO_MAGIC, sizeof(header->magic));
	header->header_size = sizeof(CSOHeader);
	header->uncompressed_size = srcSize_;
	header->sector_size = SECTOR_SIZE;
	header->version = 1;
	header->index_shift = shift_;
	header->unused[0] = 0;
	header->unused[1] = 0;

	const uint32_t sectors = static_cast<uint32_t>(srcSize_ >> SECTOR_SHIFT);

	// TODO: Need to drain first before broadcasting finish.
	// TODO: Then need to update the final index.

	uv_buf_t bufs[2];
	bufs[0] = uv_buf_init(reinterpret_cast<char *>(header), sizeof(CSOHeader));
	bufs[1] = uv_buf_init(reinterpret_cast<char *>(index_), (sectors + 1) * sizeof(uint32_t));
	const size_t totalBytes = sizeof(CSOHeader) + (sectors + 1) * sizeof(uint32_t);
	uv_.fs_write(loop_, &flush_, file_, bufs, 2, 0, [this, header, totalBytes](uv_fs_t *req) {
		if (req->result != totalBytes) {
			printf("GOT: %lld not %lld\n", (uint64_t)req->result, (uint64_t)totalBytes);
			finish_(false, "Unable to write header data");
		} else {
			finish_(true, nullptr);
		}
		uv_fs_req_cleanup(req);
		delete header;
	});
}

};