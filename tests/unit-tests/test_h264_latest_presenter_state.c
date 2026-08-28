#include "h264_latest_presenter_state.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
	struct h264_latest_state state;
	int displayed = -1;
	int recyclable = -1;
	h264_latest_state_init(&state);

	/* Idle -> immediate submit. */
	assert(h264_latest_offer(&state, 0) == -1);
	assert(state.immediate == 1 && state.in_flight == 0);
	assert(h264_latest_begin(&state) == 0);

	/* In flight -> one pending; a newer pending replaces only that frame. */
	assert(h264_latest_offer(&state, 1) == -1);
	assert(h264_latest_offer(&state, 2) == 1);
	assert(state.pending_replaced == 1);

	/* Completion makes the submitted buffer the displayed owner. */
	assert(h264_latest_complete(&state) == 0);
	recyclable = displayed;
	displayed = 0;
	assert(recyclable == -1);
	assert(state.in_flight == 2);
	assert(h264_latest_begin(&state) == 2);

	/* The old displayed decoder buffer is recyclable only after replacement. */
	assert(h264_latest_complete(&state) == 2);
	recyclable = displayed;
	displayed = 2;
	assert(recyclable == 0 && displayed == 2);
	assert(h264_latest_begin(&state) == -1);

	/* Shutdown cancels only a pending frame and never the displayed owner. */
	assert(h264_latest_offer(&state, 3) == -1);
	assert(h264_latest_offer(&state, 4) == -1);
	assert(h264_latest_cancel_pending(&state) == 4);
	assert(displayed == 2);
	assert(h264_latest_state_valid(&state));
	assert(state.max_in_flight == 1 && state.max_pending == 1);
	puts("h264_latest_presenter_state: transitions, bounds, shutdown, and lifetime pass");
	return 0;
}
