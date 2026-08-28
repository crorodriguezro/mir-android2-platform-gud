#include "h264_latest_presenter_state.h"

void h264_latest_state_init(struct h264_latest_state *state)
{
	*state = (struct h264_latest_state){.in_flight = -1, .pending = -1};
}

int h264_latest_offer(struct h264_latest_state *state, int frame)
{
	int replaced = -1;
	++state->offered;
	if (state->in_flight < 0) {
		state->in_flight = frame;
		++state->immediate;
		++state->submitted;
		state->max_in_flight = 1;
	} else if (state->pending >= 0) {
		replaced = state->pending;
		++state->pending_replaced;
		state->pending = frame;
	} else {
		++state->pending_stored;
		state->pending = frame;
	}
	if (state->pending >= 0) state->max_pending = 1;
	return replaced;
}

int h264_latest_begin(struct h264_latest_state *state)
{
	int frame;
	if (state->in_flight < 0 || state->started) return -1;
	frame = state->in_flight;
	state->started = 1;
	return frame;
}

int h264_latest_complete(struct h264_latest_state *state)
{
	int frame = state->in_flight;
	if (frame < 0 || !state->started) return -1;
	++state->completed;
	state->started = 0;
	state->in_flight = state->pending;
	state->pending = -1;
	if (state->in_flight >= 0) ++state->submitted;
	return frame;
}

int h264_latest_cancel_pending(struct h264_latest_state *state)
{
	int frame = state->pending;
	state->pending = -1;
	return frame;
}

int h264_latest_state_valid(struct h264_latest_state const *state)
{
	return state->in_flight >= -1 && state->pending >= -1 &&
		(state->started == 0 || state->started == 1) &&
		(!state->started || state->in_flight >= 0) &&
		state->max_in_flight <= 1 && state->max_pending <= 1 &&
		state->completed <= state->submitted &&
		state->submitted <= state->offered;
}
