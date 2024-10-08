/******************************************************************
** transmit.c **************************************************************
****************************************************************************
*
* functions that prepare IR codes for transmitting
*
* Copyright (C) 1999-2004 Christoph Bartelmus <lirc@bartelmus.de>
*
*/

/**
 * @file transmit.c
 * @brief Implements transmit.h
 * @author Christoph Bartelmus
 */

#include <config.h>

/* if the gap is lower than this value, we will concatenate the
 * signals and send the signal chain at a single blow */
#define LIRCD_EXACT_GAP_THRESHOLD 10000

#include "media/lirc.h"

#include "lirc/lirc_log.h"
#include "lirc/transmit.h"

static const logchannel_t logchannel = LOG_LIB;

struct sbuf {
    lirc_t* data;

    lirc_t	_data[WBUF_SIZE]; /**< Actual sending data. */
    int	wptr;
    int	too_long;
    int	is_biphase;
    lirc_t	pendingp;
    lirc_t	pendings;
    lirc_t	sum;
};

static void send_signals(struct sbuf *, lirc_t* signals, int n);
static int init_send_or_sim(struct sbuf * send_buffer, struct ir_remote* remote, struct ir_ncode* code, int sim, int repeat_preset);

/*
 * sending stuff
 */

/**
 * Initializes the sending buffer. (Just fills it with zeros.)
 */
struct sbuf* send_buffer_init()
{
    struct sbuf* send_buffer = malloc(sizeof(struct sbuf));
    if (send_buffer != NULL) {
        memset(send_buffer, 0, sizeof(struct sbuf));
    }
    return send_buffer;
}

void clear_send_buffer(struct sbuf * send_buffer)
{
	log_trace2("clearing transmit buffer");
	send_buffer->wptr = 0;
	send_buffer->too_long = 0;
	send_buffer->is_biphase = 0;
	send_buffer->pendingp = 0;
	send_buffer->pendings = 0;
	send_buffer->sum = 0;
}

void add_send_buffer(struct sbuf * send_buffer, lirc_t data)
{
	if (send_buffer->wptr < WBUF_SIZE) {
		log_trace2("adding to transmit buffer: %u", data);
		send_buffer->sum += data;
		send_buffer->_data[send_buffer->wptr] = data;
		send_buffer->wptr++;
	} else {
		send_buffer->too_long = 1;
	}
}

static void send_pulse(struct sbuf * send_buffer, lirc_t data)
{
	if (data != 0) {
		if (send_buffer->pendingp > 0) {
			send_buffer->pendingp += data;
		} else {
			if (send_buffer->pendings > 0) {
				add_send_buffer(send_buffer, send_buffer->pendings);
				send_buffer->pendings = 0;
			}
			send_buffer->pendingp = data;
		}
	}
}

static void send_space(struct sbuf * send_buffer, lirc_t data)
{
	if (data != 0) {
		if (send_buffer->wptr == 0 && send_buffer->pendingp == 0) {
			log_trace("first signal is a space!");
			return;
		}
		if (send_buffer->pendings > 0) {
			send_buffer->pendings += data;
		} else {
			if (send_buffer->pendingp > 0) {
				add_send_buffer(send_buffer, send_buffer->pendingp);
				send_buffer->pendingp = 0;
			}
			send_buffer->pendings = data;
		}
	}
}

static int bad_send_buffer(struct sbuf * send_buffer)
{
	if (send_buffer->too_long != 0)
		return 1;
	if (send_buffer->wptr == WBUF_SIZE && send_buffer->pendingp > 0)
		return 1;
	return 0;
}

static int check_send_buffer(struct sbuf * send_buffer)
{
	int i;

	if (send_buffer->wptr == 0) {
		log_trace("nothing to send");
		return 0;
	}
	for (i = 0; i < send_buffer->wptr; i++) {
		if (send_buffer->data[i] == 0) {
			if (i % 2) {
				log_trace("invalid space: %d", i);
			} else {
				log_trace("invalid pulse: %d", i);
			}
			return 0;
		}
	}

	return 1;
}

static void flush_send_buffer(struct sbuf * send_buffer)
{
	if (send_buffer->pendingp > 0) {
		add_send_buffer(send_buffer, send_buffer->pendingp);
		send_buffer->pendingp = 0;
	}
	if (send_buffer->pendings > 0) {
		add_send_buffer(send_buffer, send_buffer->pendings);
		send_buffer->pendings = 0;
	}
}

static void sync_send_buffer(struct sbuf * send_buffer)
{
	if (send_buffer->pendingp > 0) {
		add_send_buffer(send_buffer, send_buffer->pendingp);
		send_buffer->pendingp = 0;
	}
	if (send_buffer->wptr > 0 && send_buffer->wptr % 2 == 0)
		send_buffer->wptr--;
}

static void send_header(struct sbuf * send_buffer, struct ir_remote* remote)
{
	if (has_header(remote)) {
		send_pulse(send_buffer, remote->phead);
		send_space(send_buffer, remote->shead);
	}
}

static void send_foot(struct sbuf * send_buffer, struct ir_remote* remote)
{
	if (has_foot(remote)) {
		send_space(send_buffer, remote->sfoot);
		send_pulse(send_buffer, remote->pfoot);
	}
}

static void send_lead(struct sbuf * send_buffer, struct ir_remote* remote)
{
	if (remote->plead != 0)
		send_pulse(send_buffer, remote->plead);
}

static void send_trail(struct sbuf * send_buffer, struct ir_remote* remote)
{
	if (remote->ptrail != 0)
		send_pulse(send_buffer, remote->ptrail);
}

static void send_data(struct sbuf * send_buffer, struct ir_remote* remote, ir_code data, int bits, int done)
{
	int i;
	int all_bits = bit_count(remote);
	int toggle_bit_mask_bits = bits_set(remote->toggle_bit_mask);
	ir_code mask;

	data = reverse(data, bits);
	if (is_rcmm(remote)) {
		mask = 1 << (all_bits - 1 - done);
		if (bits % 2 || done % 2) {
			log_error("invalid bit number.");
			return;
		}
		for (i = 0; i < bits; i += 2, mask >>= 2) {
			switch (data & 3) {
			case 0:
				send_pulse(send_buffer, remote->pzero);
				send_space(send_buffer, remote->szero);
				break;
			/* 2 and 1 swapped due to reverse() */
			case 2:
				send_pulse(send_buffer, remote->pone);
				send_space(send_buffer, remote->sone);
				break;
			case 1:
				send_pulse(send_buffer, remote->ptwo);
				send_space(send_buffer, remote->stwo);
				break;
			case 3:
				send_pulse(send_buffer, remote->pthree);
				send_space(send_buffer, remote->sthree);
				break;
			}
			data = data >> 2;
		}
		return;
	} else if (is_xmp(remote)) {
		if (bits % 4 || done % 4) {
			log_error("invalid bit number.");
			return;
		}
		for (i = 0; i < bits; i += 4) {
			ir_code nibble;

			nibble = reverse(data & 0xf, 4);
			send_pulse(send_buffer, remote->pzero);
			send_space(send_buffer, remote->szero + nibble * remote->sone);
			data >>= 4;
		}
		return;
	}

	mask = ((ir_code)1) << (all_bits - 1 - done);
	for (i = 0; i < bits; i++, mask >>= 1) {
		if (has_toggle_bit_mask(remote) && mask & remote->toggle_bit_mask) {
			if (toggle_bit_mask_bits == 1) {
				/* backwards compatibility */
				data &= ~((ir_code)1);
				if (remote->toggle_bit_mask_state & mask)
					data |= (ir_code)1;
			} else {
				if (remote->toggle_bit_mask_state & mask)
					data ^= (ir_code)1;
			}
		}
		if (has_toggle_mask(remote) && mask & remote->toggle_mask && remote->toggle_mask_state % 2)
			data ^= 1;
		if (data & 1) {
			if (is_biphase(remote)) {
				if (mask & remote->rc6_mask) {
					send_space(send_buffer, 2 * remote->sone);
					send_pulse(send_buffer, 2 * remote->pone);
				} else {
					send_space(send_buffer, remote->sone);
					send_pulse(send_buffer, remote->pone);
				}
			} else if (is_space_first(remote)) {
				send_space(send_buffer, remote->sone);
				send_pulse(send_buffer, remote->pone);
			} else {
				send_pulse(send_buffer, remote->pone);
				send_space(send_buffer, remote->sone);
			}
		} else {
			if (mask & remote->rc6_mask) {
				send_pulse(send_buffer, 2 * remote->pzero);
				send_space(send_buffer, 2 * remote->szero);
			} else if (is_space_first(remote)) {
				send_space(send_buffer, remote->szero);
				send_pulse(send_buffer, remote->pzero);
			} else {
				send_pulse(send_buffer, remote->pzero);
				send_space(send_buffer, remote->szero);
			}
		}
		data = data >> 1;
	}
}

static void send_pre(struct sbuf * send_buffer, struct ir_remote* remote)
{
	if (has_pre(remote)) {
		send_data(send_buffer, remote, remote->pre_data, remote->pre_data_bits, 0);
		if (remote->pre_p > 0 && remote->pre_s > 0) {
			send_pulse(send_buffer, remote->pre_p);
			send_space(send_buffer, remote->pre_s);
		}
	}
}

static void send_post(struct sbuf * send_buffer, struct ir_remote* remote)
{
	if (has_post(remote)) {
		if (remote->post_p > 0 && remote->post_s > 0) {
			send_pulse(send_buffer, remote->post_p);
			send_space(send_buffer, remote->post_s);
		}
		send_data(send_buffer, remote, remote->post_data, remote->post_data_bits, remote->pre_data_bits + remote->bits);
	}
}

static void send_repeat(struct sbuf * send_buffer, struct ir_remote* remote)
{
	send_lead(send_buffer, remote);
	send_pulse(send_buffer, remote->prepeat);
	send_space(send_buffer, remote->srepeat);
	send_trail(send_buffer, remote);
}

static void send_code(struct sbuf * send_buffer, struct ir_remote* remote, ir_code code, int repeat)
{
	if (!repeat || !(remote->flags & NO_HEAD_REP))
		send_header(send_buffer, remote);
	send_lead(send_buffer, remote);
	send_pre(send_buffer, remote);
	send_data(send_buffer, remote, code, remote->bits, remote->pre_data_bits);
	send_post(send_buffer, remote);
	send_trail(send_buffer, remote);
	if (!repeat || !(remote->flags & NO_FOOT_REP))
		send_foot(send_buffer, remote);

	if (!repeat && remote->flags & NO_HEAD_REP && remote->flags & CONST_LENGTH)
		send_buffer->sum -= remote->phead + remote->shead;
}

static void send_signals(struct sbuf * send_buffer, lirc_t* signals, int n)
{
	int i;

	for (i = 0; i < n; i++)
		add_send_buffer(send_buffer, signals[i]);
}

int send_buffer_put(struct sbuf * send_buffer, struct ir_remote* remote, struct ir_ncode* code)
{
	return init_send_or_sim(send_buffer, remote, code, 0, 0);
}

/**
 * Do not document this function
 * @cond
 */
int init_sim(struct sbuf * send_buffer, struct ir_remote* remote, struct ir_ncode* code, int repeat_preset)
{
	return init_send_or_sim(send_buffer, remote, code, 1, repeat_preset);
}
/**
 *@endcond
 */


int send_buffer_length(struct sbuf * send_buffer)
{
	return send_buffer->wptr;
}


const lirc_t* send_buffer_data(struct sbuf * send_buffer)
{
	return send_buffer->data;
}

lirc_t send_buffer_sum(struct sbuf * send_buffer)
{
	return send_buffer->sum;
}

static int init_send_or_sim(struct sbuf * send_buffer, struct ir_remote* remote, struct ir_ncode* code, int sim, int repeat_preset)
{
	int i, repeat = repeat_preset;

	if (is_grundig(remote) || is_serial(remote) || is_bo(remote)) {
		if (!sim)
			log_error("sorry, can't send this protocol yet");
		return 0;
	}
	clear_send_buffer(send_buffer);
	if (strcmp(remote->name, "lirc") == 0) {
		send_buffer->data[send_buffer->wptr] = LIRC_EOF | 1;
		send_buffer->wptr += 1;
		goto final_check;
	}

	if (is_biphase(remote))
		send_buffer->is_biphase = 1;
	if (!sim) {
		if (repeat_remote == NULL)
			remote->repeat_countdown = remote->min_repeat;
		else
			repeat = 1;
	}

init_send_loop:
	if (repeat && has_repeat(remote) && code->next == NULL) {
		if (remote->flags & REPEAT_HEADER && has_header(remote))
			send_header(send_buffer, remote);
		send_repeat(send_buffer, remote);
	} else {
		if (!is_raw(remote)) {
			ir_code next_code;

			if (sim || code->transmit_state == NULL)
				next_code = code->code;
			else
				next_code = code->transmit_state->code;

			if (repeat && has_repeat_mask(remote))
				next_code ^= remote->repeat_mask;

			send_code(send_buffer, remote, next_code, repeat);
			if (!sim && has_toggle_mask(remote)) {
				remote->toggle_mask_state++;
				if (remote->toggle_mask_state == 4)
					remote->toggle_mask_state = 2;
			}
			send_buffer->data = send_buffer->_data;
		} else {
			if (code->signals == NULL) {
				if (!sim)
					log_error("no signals for raw send");
				return 0;
			}
			if (send_buffer->wptr > 0) {
				send_signals(send_buffer, code->signals, code->length);
			} else {
				send_buffer->data = code->signals;
				send_buffer->wptr = code->length;
				for (i = 0; i < code->length; i++)
					send_buffer->sum += code->signals[i];
			}
		}
	}
	sync_send_buffer(send_buffer);
	if (bad_send_buffer(send_buffer)) {
		if (!sim)
			log_error("buffer too small");
		return 0;
	}
	if (sim)
		goto final_check;

	if (has_repeat_gap(remote) && repeat && has_repeat(remote)) {
		remote->min_remaining_gap = remote->repeat_gap;
		remote->max_remaining_gap = remote->repeat_gap;
	} else if (is_const(remote)) {
		lirc_t sum = send_buffer->sum + send_buffer->pendings;
		if (min_gap(remote) > sum) {
			remote->min_remaining_gap = min_gap(remote) - sum;
			remote->max_remaining_gap = max_gap(remote) - sum;
		} else {
			log_error("too short gap: %u", remote->gap);
			remote->min_remaining_gap = min_gap(remote);
			remote->max_remaining_gap = max_gap(remote);
			return 0;
		}
	} else {
		remote->min_remaining_gap = min_gap(remote);
		remote->max_remaining_gap = max_gap(remote);
	}
	/* update transmit state */
	if (code->next != NULL) {
		if (code->transmit_state == NULL) {
			code->transmit_state = code->next;
		} else {
			code->transmit_state = code->transmit_state->next;
			if (is_xmp(remote) && code->transmit_state == NULL)
				code->transmit_state = code->next;
		}
	}
	if ((remote->repeat_countdown > 0 || code->transmit_state != NULL)
	    && remote->min_remaining_gap < LIRCD_EXACT_GAP_THRESHOLD) {
		if (send_buffer->data != send_buffer->_data) {
			lirc_t* signals;
			int n;

			log_trace("unrolling raw signal optimisation");
			signals = send_buffer->data;
			n = send_buffer->wptr;
			send_buffer->data = send_buffer->_data;
			send_buffer->wptr = 0;

			send_signals(send_buffer, signals, n);
		}
		log_trace("concatenating low gap signals");
		if (code->next == NULL || code->transmit_state == NULL)
			remote->repeat_countdown--;
		send_space(send_buffer, remote->min_remaining_gap);
		flush_send_buffer(send_buffer);
		send_buffer->sum = 0;

		repeat = 1;
		goto init_send_loop;
	}
	log_trace2("transmit buffer ready");

final_check:
	if (!check_send_buffer(send_buffer)) {
		if (!sim) {
			log_error("invalid send buffer");
			log_error("this remote configuration cannot be used to transmit");
		}
		return 0;
	}
	return 1;
}
