// license:BSD-3-Clause
// copyright-holders:OpenAI
/***************************************************************************

    High-level external XT keyboard for the IBM Simon bottom connector.

    Simon's VG230 uses the PC/XT serial interface, not an AT/PS2 controller.
    Retain MAME's PC keyboard matrix, make/break and typematic behavior, but
    serialize each scan byte as a start 1 followed by eight LSB-first bits.
    Falling KBDCLK edges clock the receiver.  The nominal clock is 10 kHz;
    the 2 ms inter-byte gap is an external-keyboard timing approximation.

    KBDCLK is open-collector: the motherboard can inhibit transmission.
    A low of at least 20 ms requests an XT keyboard reset/self-test.  This
    is a protocol model, not a dump of a particular accessory's firmware.
    There is no PS/2 command channel, ACK byte or 8042 translation here.

    As in MAME's PC/XT motherboard receiver, a full receive latch holds
    KBDATA low until the host acknowledges it.  This hardware handshake
    delays the next frame, not the final rising clock of the current one.
    It is an XT-compatible model; this VG230 detail has not been measured.
    It does not imply that Simon software can arbitrarily drive KBDATA.

***************************************************************************/

#include "emu.h"
#include "simon_kbd.h"

DEFINE_DEVICE_TYPE(SIMON_XT_KEYBOARD, simon_xt_keyboard_device, "simon_xt_keyboard", "Simon External XT Keyboard")

static INPUT_PORTS_START(simon_xt_keyboard)
	PORT_INCLUDE(pc_keyboard)

	// These host function keys operate Simon's existing front-panel controls.
	// The external keyboard keys remain available for explicit remapping.
	PORT_MODIFY("pc_keyboard_4")
	PORT_BIT(0x0002, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("F7") PORT_CHAR(UCHAR_MAMEKEY(F7))
	PORT_BIT(0x0004, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("F8") PORT_CHAR(UCHAR_MAMEKEY(F8))
	PORT_BIT(0x0008, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("F9") PORT_CHAR(UCHAR_MAMEKEY(F9))
	PORT_BIT(0x0010, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("F10") PORT_CHAR(UCHAR_MAMEKEY(F10))
	// PgUp/PgDn control Simon's volume; the physical keypad keys still work.
	PORT_BIT(0x0200, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("KP 9 (PgUp)") PORT_CODE(KEYCODE_9_PAD)

	PORT_MODIFY("pc_keyboard_5")
	PORT_BIT(0x0002, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("KP 3 (PgDn)") PORT_CODE(KEYCODE_3_PAD)
INPUT_PORTS_END

simon_xt_keyboard_device::simon_xt_keyboard_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	pc_keyboard_device(mconfig, SIMON_XT_KEYBOARD, tag, owner, clock),
	m_out_clock(*this),
	m_out_data(*this)
{
	m_type = KEYBOARD_TYPE::PC;
}

ioport_constructor simon_xt_keyboard_device::device_input_ports() const
{
	return INPUT_PORTS_NAME(simon_xt_keyboard);
}

void simon_xt_keyboard_device::device_start()
{
	pc_keyboard_device::device_start();
	m_serial_timer = timer_alloc(FUNC(simon_xt_keyboard_device::serial_tick), this);
	save_item(NAME(m_inhibit_started));
	save_item(NAME(m_shift));
	save_item(NAME(m_bits_remaining));
	save_item(NAME(m_connected));
	save_item(NAME(m_host_release));
	save_item(NAME(m_host_data_release));
	save_item(NAME(m_clock));
	save_item(NAME(m_data));
	machine().save().register_postload(save_prepost_delegate(FUNC(simon_xt_keyboard_device::postload), this));
}

void simon_xt_keyboard_device::device_reset()
{
	// The base reset clears held-key/repeat state and queues the XT BAT byte AA.
	pc_keyboard_device::device_reset();
	m_shift = 0;
	m_bits_remaining = 0;
	m_serial_timer->adjust(attotime::never);
	clock_output(1);
	data_output(1);
	if (!m_connected)
	{
		enable(0);
		clear_buffer();
	}
	else if (m_host_release && m_host_data_release)
		m_serial_timer->adjust(attotime::from_msec(2));
	else if (!m_host_release)
		m_inhibit_started = machine().time();
}

void simon_xt_keyboard_device::set_connected(bool connected)
{
	if (m_connected == connected)
		return;
	m_connected = connected;
	if (connected)
		reset();
	else
	{
		enable(0);
		clear_buffer();
		m_shift = 0;
		m_bits_remaining = 0;
		m_serial_timer->adjust(attotime::never);
		clock_output(1);
		data_output(1);
	}
}

void simon_xt_keyboard_device::clock_write(int host_release)
{
	const bool released = bool(host_release);
	if (m_host_release == released)
		return;
	m_host_release = released;
	if (!m_connected)
		return;
	if (!released)
	{
		m_inhibit_started = machine().time();
		m_serial_timer->adjust(attotime::never);
	}
	else if (machine().time() - m_inhibit_started >= attotime::from_msec(20))
		reset();
	else if (m_bits_remaining)
		// Short inhibit pauses the current frame without clearing keys or BAT.
		m_serial_timer->adjust(attotime::from_usec(50));
	else if (m_host_data_release)
		m_serial_timer->adjust(attotime::from_msec(2));
}

void simon_xt_keyboard_device::data_write(int host_release)
{
	const bool released = bool(host_release);
	if (m_host_data_release == released)
		return;
	m_host_data_release = released;
	if (!m_connected || !m_host_release || m_bits_remaining)
		return;

	// The receiver may assert hold from inside the last falling-edge callback.
	// Let that frame finish; only an idle transmitter waits for hardware ACK.
	m_serial_timer->adjust(released ? attotime::from_msec(2) : attotime::never);
}

void simon_xt_keyboard_device::clock_output(int state)
{
	if (m_clock != bool(state))
	{
		m_clock = bool(state);
		m_out_clock(state);
	}
}

void simon_xt_keyboard_device::data_output(int state)
{
	if (m_data != bool(state))
	{
		m_data = bool(state);
		m_out_data(state);
	}
}

void simon_xt_keyboard_device::postload()
{
	m_out_data(m_data);
	m_out_clock(m_clock);
}

TIMER_CALLBACK_MEMBER(simon_xt_keyboard_device::serial_tick)
{
	if (!m_connected || !m_host_release)
		return;

	if (!m_bits_remaining)
	{
		if (!m_host_data_release)
			return;

		// Zero is the base device's empty-queue sentinel, not a PC scan code.
		const uint8_t scan = read();
		if (!scan)
		{
			m_serial_timer->adjust(attotime::from_msec(1));
			return;
		}
		m_shift = (uint16_t(scan) << 1) | 1;
		m_bits_remaining = 9;
		data_output(BIT(m_shift, 0));
		clock_output(1);
	}
	else if (m_clock)
		clock_output(0);
	else
	{
		clock_output(1);
		m_shift >>= 1;
		if (!--m_bits_remaining)
		{
			data_output(1);
			m_serial_timer->adjust(m_host_data_release ? attotime::from_msec(2) : attotime::never);
			return;
		}
		data_output(BIT(m_shift, 0));
	}
	m_serial_timer->adjust(attotime::from_usec(50));
}
