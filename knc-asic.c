/*
 * cgminer driver for KnCminer Neptune devices
 *
 * Copyright 2014 KnCminer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <string.h>

#include <zlib.h>

#include "miner.h"
#include "logging.h"

#include "knc-transport.h"

#include "knc-asic.h"


#define MAX_ASICS               6
#define DIES_PER_ASIC           4
#define MAX_CORES_PER_DIE       360
#define WORKS_PER_CORE          2

/* ASIC Command codes */
#define	ASIC_CMD_GETINFO             0x80
#define ASIC_CMD_SETWORK             0x81
#define ASIC_CMD_SETWORK_CLEAN       0x83        /* Neptune */
#define ASIC_CMD_HALT                0x83        /* Jupiter */
#define ASIC_CMD_REPORT              0x82

#define ASIC_ACK_CRC                    (1<<5)
#define ASIC_ACK_ACCEPT                 (1<<2)
#define ASIC_ACK_MASK                   (~(ASIC_ACK_CRC|ASIC_ACK_ACCEPT))
#define ASIC_ACK_MATCH                  ((1<<7)|(1<<0))

#define ASIC_VERSION_JUPITER            0xa001
#define ASIC_VERSION_NEPTUNE            0xa002

/* Control Commands
 *
 * SPI command on channel. 1-
 *   1'b1 3'channel 12'msglen_in_bits SPI message data
 * Sends the supplied message on selected SPI bus
 *
 * Communication test
 *   16'h1 16'x
 * Simple test of SPI communication
 *
 * LED control
 *   4'h1 4'red 4'green 4'blue
 * Sets led colour
 *
 * Clock frequency
 *   4'h2 12'msglen_in_bits 4'channel 4'die 16'MHz 512'x
 * Configures the hashing clock rate
 */

/* ASIC Command structure
 * command      8 bits
 * chip         8 bits
 * core         16 bits
 * data         [command dependent]
 * CRC32        32 bits (Neptune)
 *
 * ASIC response starts immediately after core address bits.
 *
 * response data
 * CRC32        32 bits (Neptune)
 * STATUS       8 bits   1 0 ~CRC_OK 0 0 ACCEPTED_WORK 0 1 (Neptune)
 *
 * Requests
 *
 * SETWORK (Jupiter)
 * midstate     256 bits
 * data         96 bits
 *
 * SETWORK/SETWORK_CLEAN (Neptune)
 * slot | 0xf0  8 bits
 * precalc_midstate  192 bits
 * precalc_data 96 bits
 * midstate     256 bits
 *
 * Returns REPORT response on Neptune
 *
 * Responses
 *
 * GETINFO
 *
 * (core field unused)
 *
 * cores        16 bits
 * version      16 bits
 * reserved     64 bits         (Neptune)
 * core_status  cores * 2 bits  (Neptune) rounded up to bytes
 *                              1' want_work 
 *				1' has_report (unreliable)
 *
 * REPORT
 *
 * reserved     2 bits
 * next_state   1 bit   next work state loaded
 * state        1 bit   hashing  (0 on Jupiter)
 * next_slot    4 bit   slot id of next work state (0 on Jupiter)
 * progress     8 bits  upper 8 bits of nonce counter
 * active_slot  4 bits  slot id of current work state
 * nonce_slot   4 bits  slot id of found nonce
 * nonce        32 bits
 * 
 * reserved     4 bits
 * nonce_slot   4 bits
 * nonce        32 bits
 *
 * repeat for 5 nonce entries in total on Neptune
 * Jupiter only has first nonce entry
 */

// Precalculate first 3 rounds of SHA256 - as much as possible	
// Macro routines copied from sha2.c
static void knc_prepare_neptune_work(unsigned char *out, struct work *work) {
        const uint8_t *midstate = work->midstate;
        const uint8_t *data = work->data + 16*4;

#ifndef GET_ULONG_BE
#define GET_ULONG_BE(b,i)                             \
		(( (uint32_t) (b)[(i)    ] << 24 )	\
                | ( (uint32_t) (b)[(i) + 1] << 16 )	\
                | ( (uint32_t) (b)[(i) + 2] <<  8 )	\
                | ( (uint32_t) (b)[(i) + 3]       ))
#endif

#ifndef GET_ULONG_LE
#define GET_ULONG_LE(b,i)                             \
		(( (uint32_t) (b)[(i) + 3] << 24 )	\
                | ( (uint32_t) (b)[(i) + 2] << 16 )	\
                | ( (uint32_t) (b)[(i) + 1] <<  8 )	\
                | ( (uint32_t) (b)[(i) + 0]       ))
#endif

#ifndef PUT_ULONG_BE
#define PUT_ULONG_BE(n,b,i)                             \
	{						\
		(b)[(i)    ] = (unsigned char) ( (n) >> 24 );	\
		(b)[(i) + 1] = (unsigned char) ( (n) >> 16 );	\
		(b)[(i) + 2] = (unsigned char) ( (n) >>  8 );	\
		(b)[(i) + 3] = (unsigned char) ( (n)       );	\
	}
#endif

#ifndef PUT_ULONG_LE
#define PUT_ULONG_LE(n,b,i)                             \
	{						\
		(b)[(i) + 3] = (unsigned char) ( (n) >> 24 );	\
		(b)[(i) + 2] = (unsigned char) ( (n) >> 16 );	\
		(b)[(i) + 1] = (unsigned char) ( (n) >>  8 );	\
		(b)[(i) + 0] = (unsigned char) ( (n)       );	\
	}
#endif

#define  SHR(x,n) ((x & 0xFFFFFFFF) >> n)
#define ROTR(x,n) (SHR(x,n) | (x << (32 - n)))

#define S0(x) (ROTR(x, 7) ^ ROTR(x,18) ^  SHR(x, 3))
#define S1(x) (ROTR(x,17) ^ ROTR(x,19) ^  SHR(x,10))

#define S2(x) (ROTR(x, 2) ^ ROTR(x,13) ^ ROTR(x,22))
#define S3(x) (ROTR(x, 6) ^ ROTR(x,11) ^ ROTR(x,25))

#define F0(x,y,z) ((x & y) | (z & (x | y)))
#define F1(x,y,z) (z ^ (x & (y ^ z)))

#define R(t)                                    \
(                                               \
    W[t] = S1(W[t -  2]) + W[t -  7] +          \
           S0(W[t - 15]) + W[t - 16]            \
)

#define P(a,b,c,d,e,f,g,h,x,K)                  \
	{					\
		temp1 = h + S3(e) + F1(e,f,g) + K + x;	\
		temp2 = S2(a) + F0(a,b,c);		\
		d += temp1; h = temp1 + temp2;		\
	}

    uint32_t temp1, temp2, W[16+3];
    uint32_t A, B, C, D, E, F, G, H;

    W[0] = GET_ULONG_LE(data,  0*4 );
    W[1] = GET_ULONG_LE(data,  1*4 );
    W[2] = GET_ULONG_LE(data,  2*4 );
    W[3] = 0;                 // since S0(0)==0, this must be 0. S0(nonce) is added in hardware.
    W[4] = 0x80000000;
    W[5] = 0;
    W[6] = 0;
    W[7] = 0;
    W[8] = 0;
    W[9] = 0;
    W[10] = 0;
    W[11] = 0;
    W[12] = 0;
    W[13] = 0;
    W[14] = 0;
    W[15] = 0x00000280;
    R(16);  // Expand W 14, 9, 1, 0
    R(17);  //          15, 10, 2, 1
    R(18);  //          16, 11, 3, 2

    A = GET_ULONG_LE(midstate, 0*4 );
    B = GET_ULONG_LE(midstate, 1*4 );
    C = GET_ULONG_LE(midstate, 2*4 );
    D = GET_ULONG_LE(midstate, 3*4 );
    E = GET_ULONG_LE(midstate, 4*4 );
    F = GET_ULONG_LE(midstate, 5*4 );
    G = GET_ULONG_LE(midstate, 6*4 );
    H = GET_ULONG_LE(midstate, 7*4 );

    uint32_t D_ = D, H_ = H;
    P( A, B, C, D_, E, F, G, H_, W[ 0], 0x428A2F98 );
    uint32_t C_ = C, G_ = G;
    P( H_, A, B, C_, D_, E, F, G_, W[ 1], 0x71374491 );
    uint32_t B_ = B, F_ = F;
    P( G_, H_, A, B_, C_, D_, E, F_, W[ 2], 0xB5C0FBCF );

    PUT_ULONG_BE( D_, out, 0*4 );
    PUT_ULONG_BE( C_, out, 1*4 );
    PUT_ULONG_BE( B_, out, 2*4 );
    PUT_ULONG_BE( H_, out, 3*4 );
    PUT_ULONG_BE( G_, out, 4*4 );
    PUT_ULONG_BE( F_, out, 5*4 );
    PUT_ULONG_BE( W[18], out, 6*4 );  // This is partial S0(nonce) added by hardware
    PUT_ULONG_BE( W[17], out, 7*4 );
    PUT_ULONG_BE( W[16], out, 8*4 );
    PUT_ULONG_BE( H, out, 9*4 );
    PUT_ULONG_BE( G, out, 10*4 );
    PUT_ULONG_BE( F, out, 11*4 );
    PUT_ULONG_BE( E, out, 12*4 );
    PUT_ULONG_BE( D, out, 13*4 );
    PUT_ULONG_BE( C, out, 14*4 );
    PUT_ULONG_BE( B, out, 15*4 );
    PUT_ULONG_BE( A, out, 16*4 );
}

static void knc_prepare_jupiter_work(unsigned char *out, struct work *work) {
        int i;
        for (i = 0; i < 8 * 4; i++)
                out[i] = work->midstate[8 * 4 - i - 1];
        for (i = 0; i < 3 * 4; i++)
                out[8 * 4 + i] = work->data[16 * 4 + 3 * 4 - i - 1];
}

static void knc_prepare_core_command(uint8_t *request, int command, int die, int core)
{
	request[0] = command;
	request[1] = die;
	request[2] = core >> 8;
	request[3] = core & 0xff;
}

int knc_prepare_report(uint8_t *request, int die, int core)
{
	knc_prepare_core_command(request, ASIC_CMD_REPORT, die, core);
	return 4;
}

int knc_prepare_neptune_setwork(uint8_t *request, int die, int core, int slot, struct work *work, int clean)
{
	if (!clean)
		knc_prepare_core_command(request, ASIC_CMD_SETWORK, die, core);
	else
		knc_prepare_core_command(request, ASIC_CMD_SETWORK_CLEAN, die, core);
	request[4] = slot | 0xf0;
	if (work)
		knc_prepare_neptune_work(request + 4 + 1, work);
	else
		memset(request + 4 + 1, 0, 6*4 + 3*4 + 8*4);
	return 4 + 1 + 6*4 + 3*4 + 8*4;
}

int knc_prepare_jupiter_setwork(uint8_t *request, int die, int core, int slot, struct work *work)
{
	knc_prepare_core_command(request, ASIC_CMD_SETWORK, die, core);
	request[4] = slot | 0xf0;
	if (work)
		knc_prepare_jupiter_work(request + 4 + 1, work);
	else
		memset(request + 4 + 1, 0, 8*4 + 3*4);
	return 4 + 1 + 8*4 + 3*4;
}

int knc_prepare_jupiter_halt(uint8_t *request, int die, int core)
{
	knc_prepare_core_command(request, ASIC_CMD_HALT, die, core);
	return 4;
}

int knc_prepare_neptune_halt(uint8_t *request, int die, int core)
{
	knc_prepare_core_command(request, ASIC_CMD_HALT, die, core);
	request[4] = 0 | 0xf0;
	memset(request + 4 + 1, 0, 6*4 + 3*4 + 8*4);
	return 4 + 1 + 6*4 + 3*4 + 8*4;
}

void knc_prepare_neptune_message(int request_length, const uint8_t *request, uint8_t *buffer)
{
    uint32_t crc;
    memcpy(buffer, request, request_length);
    buffer += request_length;
    crc = crc32(0, Z_NULL, 0);
    crc = crc32(crc, request, request_length);
    PUT_ULONG_BE(crc, buffer, 0);
}

int knc_prepare_transfer(uint8_t *txbuf, int offset, int size, int channel, int request_length, const uint8_t *request, int response_length)
{
	/* FPGA control, request header, request body/response, CRC(4), ACK(1), EXTRA(3) */
        int msglen = MAX(request_length, 4 + response_length ) + 4 + 1 + 3;
        int len = 2 + msglen;
	txbuf += offset;

	if (len + offset > size) {
		applog(LOG_DEBUG, "KnC SPI buffer full");
		return -1;
	}
	txbuf[0] = 1 << 7 | (channel+1) << 4 | (msglen * 8) >> 8;
	txbuf[1] = (msglen * 8);
	knc_prepare_neptune_message(request_length, request, txbuf+2);

	return len;
}

int knc_verify_response(uint8_t *rxbuf, int len, int response_length)
{
    int ret = 0;
    if (response_length > 0) {
        uint32_t crc, recv_crc;
	crc = crc32(0, Z_NULL, 0);
        crc = crc32(crc, rxbuf + 2 + 4, response_length);
	recv_crc = GET_ULONG_BE(rxbuf + 2 + 4, response_length);
	if (crc != recv_crc)
                ret |= KNC_ERR_CRC;
    }
    uint8_t ack = rxbuf[len - 4]; /* 2 + MAX(4 + response_length, request_length) + 4; */

    if ((ack & ASIC_ACK_MASK) != ASIC_ACK_MATCH)
        ret |= KNC_ERR_ACK;
    if ((ack & ASIC_ACK_CRC))
        ret |= KNC_ERR_CRCACK;
    if ((ack & ASIC_ACK_ACCEPT))
        ret |= KNC_ACCEPTED;
    return ret;
}

int knc_syncronous_transfer(void *ctx, int channel, int request_length, const uint8_t *request, int response_length, uint8_t *response)
{
    /* FPGA control, request header, request body/response, CRC(4), ACK(1), EXTRA(3) */
    int msglen = MAX(request_length, 4 + response_length ) + 4 + 1 + 3;
    int len = 2 + msglen;
    uint8_t txbuf[len];
    uint8_t rxbuf[len];
    memset(txbuf, 0, len);
    knc_prepare_transfer(txbuf, 0, len, channel, request_length, request, response_length);
    knc_trnsp_transfer(ctx, txbuf, rxbuf, len);

    if (response_length > 0)
	memcpy(response, rxbuf + 2 + 4, response_length);

    return knc_verify_response(rxbuf, len, response_length);
}

int knc_decode_info(uint8_t *response, struct knc_die_info *die_info)
{
	int cores_in_die = response[0]<<8 | response[1];
	int version = response[2]<<8 | response[3];
	if (version == ASIC_VERSION_JUPITER && cores_in_die <= 48) {
		die_info->version = KNC_VERSION_JUPITER;
		die_info->cores = cores_in_die;
		memset(die_info->want_work, -1, cores_in_die);
		return 0;
	} else if (version == ASIC_VERSION_NEPTUNE && cores_in_die <= MAX_CORES_PER_DIE) {
		die_info->version = KNC_VERSION_NEPTUNE;
		die_info->cores = cores_in_die;
		int core;
		for (core = 0; core < cores_in_die; core++)
			die_info->want_work[core] = ((response[12 + core/4] >> ((3-(core % 4)) * 2)) >> 1) & 1;
		return 0;
	} else {
		return -1;
	}
}

int knc_detect_die(void *ctx, int channel, int die, struct knc_die_info *die_info)
{
	uint8_t get_info[4] = { ASIC_CMD_GETINFO, die, 0, 0 };
	int response_len = 2 + 2 + 4 + 4 + (MAX_CORES_PER_DIE*2 + 7) / 8;
	uint8_t response[response_len];
	int status = knc_syncronous_transfer(ctx, channel, 4, get_info, response_len, response);
	/* Workaround for pre-ASIC version */
	int cores_in_die = response[0]<<8 | response[1];
	int version = response[2]<<8 | response[3];
	if (version == ASIC_VERSION_NEPTUNE && cores_in_die < MAX_CORES_PER_DIE) {
		applog(LOG_DEBUG, "KnC %d-%d: Looks like a NEPTUNE die with %d cores", channel, die, cores_in_die);
		/* Try again with right response size */
		response_len = 2 + 2 + 4 + 4 + (cores_in_die*2 + 7) / 8;
		status = knc_syncronous_transfer(ctx, channel, 4, get_info, response_len, response);
	}
	int rc = -1;
	if (version == ASIC_VERSION_JUPITER || status == 0)
		rc = knc_decode_info(response, die_info);
	if (rc == 0)
		applog(LOG_INFO, "KnC %d-%d: Found %s die with %d cores", channel, die,
			die_info->version == KNC_VERSION_NEPTUNE ? "NEPTUNE" : 
			die_info->version == KNC_VERSION_JUPITER ? "JUPITER" : 
			"UNKNOWN",
			cores_in_die);
	else
		applog(LOG_DEBUG, "KnC %d-%d: No KnC chip found", channel, die);
	return rc;
}

