/**
 * This file is part of LUNA.
 *
 * Copyright (c) 2020 Great Scott Gadgets <info@greatscottgadgets.com>
 * SPDX-License-Identifier: BSD-3-Clause
 */


#include <tusb.h>
#include <apollo_board.h>
#include "spi.h"
#include "uart.h"

#include <jtag.h>

extern uint8_t jtag_in_buffer[256];
extern uint8_t jtag_out_buffer[256];

/**
 * Flags for our JTAG commands.
 */
enum {
	FLAG_ADVANCE_STATE = 0b01,
	FLAG_FORCE_BITBANG = 0b10
};


/**
 * Hook that performs hardware-specific initialization.
 */
void jtag_platform_init(void)
{
	uart_release_pinmux();

	// Ensure the TDO GPIO is continuously sampled, rather
	// than sampled on-demand. This allows us to significantly
	// speak up TDO reads.
	PORT->Group[0].CTRL.reg = (1 << TDO_GPIO);

	// Set up our SPI port for SPI-accelerated JTAG.
	spi_init(SPI_FPGA_JTAG, true, false, 1, 1, 1);
}


/**
 * Hook that performs hardware-specific deinitialization.
 */
void jtag_platform_deinit(void)
{
	// Restore use of our connection to a default of being a UART.
	uart_configure_pinmux();
}


/**
 * Request that performs the actual JTAG scan event.
 * Arguments:
 *     wValue: the number of bits to scan; total
 *     wIndex: 
 *        - 1 if the given command should advance the FSM
 *        - 2 if the given command should be sent using the slow method
 */
bool handle_jtag_request_scan(uint8_t rhport, tusb_control_request_t const* request)
{
	// Our bulk method can only send whole bytes; so send as many bytes as we can
	// using the fast method; and then send the remainder using our slow method.
	size_t bytes_to_send_bulk = request->wValue / 8;
	size_t bits_to_send_slow  = request->wValue % 8;

	// We can't handle 0-byte transfers; fail out.
	if (!bits_to_send_slow && !bytes_to_send_bulk) {
		return false;
	}

	// If this would scan more than we have buffer for, fail out.
	if (bytes_to_send_bulk > sizeof(jtag_out_buffer)) {
		return false;
	}

	// If we've been asked to send data the slow way, honor that, and send all of our bits
	// using the slow method.
	if (request->wIndex & FLAG_FORCE_BITBANG) {
		bytes_to_send_bulk = 0;
		bits_to_send_slow  = request->wValue;
	}

	// If we're going to advance state, always make sure the last bit is sent using the slow method,
	// so we can handle JTAG TAP state advancement on the last bit. If we don't have any bits to send slow,
	// send the last byte slow.
	if (!bits_to_send_slow && (request->wIndex & FLAG_ADVANCE_STATE)) {
		bytes_to_send_bulk--;
		bits_to_send_slow = 8;
	}

	// Switch to SPI mode, and send the bulk of the transfer using it.
	if (bytes_to_send_bulk) {
		spi_configure_pinmux(SPI_FPGA_JTAG);
		spi_send(SPI_FPGA_JTAG, jtag_out_buffer, jtag_in_buffer, bytes_to_send_bulk);
	}

	// Switch back to GPIO mode, and send the remainder using the slow method.
	spi_release_pinmux(SPI_FPGA_JTAG);
	if (bits_to_send_slow) {
		jtag_tap_shift(jtag_out_buffer + bytes_to_send_bulk, jtag_in_buffer + bytes_to_send_bulk,
				bits_to_send_slow, request->wIndex);
	}
	return tud_control_xfer(rhport, request, NULL, 0);
}

