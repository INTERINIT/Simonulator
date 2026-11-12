// license:BSD-3-Clause
// copyright-holders:OpenAI
#ifndef MAME_IBM_SIMON_KBD_H
#define MAME_IBM_SIMON_KBD_H

#pragma once

#include "machine/pckeybrd.h"

// High-level external PC/XT keyboard on Simon's KBDCLK/KBDATA connector.
// The keyboard generates serial pins; the VG230 owns its receive latch/IRQ1.
class simon_xt_keyboard_device : public pc_keyboard_device
{
public:
	simon_xt_keyboard_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	void set_connected(bool connected);
	bool connected() const { return m_connected; }
	void clock_write(int host_release);
	void data_write(int host_release);
	auto out_clock_cb() { return m_out_clock.bind(); }
	auto out_data_cb() { return m_out_data.bind(); }

protected:
	virtual ioport_constructor device_input_ports() const override ATTR_COLD;
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

private:
	TIMER_CALLBACK_MEMBER(serial_tick);
	void clock_output(int state);
	void data_output(int state);
	void postload();

	devcb_write_line m_out_clock;
	devcb_write_line m_out_data;
	emu_timer *m_serial_timer = nullptr;
	attotime m_inhibit_started = attotime::zero;
	uint16_t m_shift = 0;
	uint8_t m_bits_remaining = 0;
	bool m_connected = true;
	bool m_host_release = true;
	bool m_host_data_release = true;
	bool m_clock = true;
	bool m_data = true;
};

DECLARE_DEVICE_TYPE(SIMON_XT_KEYBOARD, simon_xt_keyboard_device)

#endif // MAME_IBM_SIMON_KBD_H
