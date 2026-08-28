#ifndef MIR_UTILS_H264_LATEST_PRESENTER_STATE_H_
#define MIR_UTILS_H264_LATEST_PRESENTER_STATE_H_

#include <stdint.h>

struct h264_latest_state {
	int in_flight;
	int pending;
	int started;
	uint64_t offered;
	uint64_t immediate;
	uint64_t pending_stored;
	uint64_t pending_replaced;
	uint64_t submitted;
	uint64_t completed;
	unsigned int max_in_flight;
	unsigned int max_pending;
};

void h264_latest_state_init(struct h264_latest_state *state);
int h264_latest_offer(struct h264_latest_state *state, int frame);
int h264_latest_begin(struct h264_latest_state *state);
int h264_latest_complete(struct h264_latest_state *state);
int h264_latest_cancel_pending(struct h264_latest_state *state);
int h264_latest_state_valid(struct h264_latest_state const *state);

#endif
