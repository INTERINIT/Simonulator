// license:BSD-3-Clause
// copyright-holders:OpenAI
/***************************************************************************

    IBM Simon Personal Communicator (1994)

    Early hardware bring-up driver.  Simon is built around the Vadem VG230,
    a 16 MHz NEC V30HL-based PC/XT-compatible system-on-chip.

    The current implementation models the PC/XT core and the VG230 indexed
    register and 16 KiB memory-window mechanisms.  LCD, pen digitizer and
    PCMCIA boot, touch, speaker and high-level cellular RF support are
    present.  Power management and telephone audio remain incomplete.

***************************************************************************/

#include "emu.h"

#include "cpu/nec/nec.h"
#include "machine/genpc.h"
#include "machine/ram.h"
#include "imagedev/bitbngr.h"
#include "sound/flt_rc.h"

#include "crsshair.h"
#include "emupal.h"
#include "screen.h"

#include "ibmsimon.lh"

namespace {

class ibmsimon_state : public driver_device
{
public:
	ibmsimon_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_maincpu(*this, "maincpu"),
		m_mb(*this, "mb"),
		m_mainram(*this, RAM_TAG),
		m_bios(*this, "bios"),
		m_flash(*this, "flash"),
		m_pen_x(*this, "PENX"),
		m_pen_y(*this, "PENY"),
		m_pen_button(*this, "PEN"),
		m_cellular(*this, "CELLULAR"),
		m_cellular_system(*this, "CELLULAR_SYSTEM"),
		m_cellular_link(*this, "cellular"),
		m_power_led(*this, "power_led"),
		m_phone_led(*this, "phone_led"),
		m_backlight_led(*this, "backlight_led")
	{
	}

	void ibmsimon(machine_config &config);

protected:
	virtual void machine_start() override;
	virtual void machine_reset() override;

private:
	required_device<v30_device> m_maincpu;
	required_device<pc_noppi_mb_device> m_mb;
	required_device<ram_device> m_mainram;
	required_region_ptr<u16> m_bios;
	required_region_ptr<u8> m_flash;
	required_ioport m_pen_x;
	required_ioport m_pen_y;
	required_ioport m_pen_button;
	required_ioport m_cellular;
	required_ioport m_cellular_system;
	required_device<bitbanger_device> m_cellular_link;
	output_finder<> m_power_led;
	output_finder<> m_phone_led;
	output_finder<> m_backlight_led;

	std::array<u8, 0x80000> m_upper_ram{};
	std::array<u8, 0x8000> m_video_ram{};
	std::array<u8, 0x100> m_vg230_regs{};
	std::array<u8, 0x40> m_map_low{};
	std::array<u8, 0x40> m_map_high{};
	std::array<u8, 0x100> m_crtc_regs{};
	std::array<u8, 3> m_parallel_regs{};
	std::array<u8, 4> m_dma_page_regs{};
	u8 m_vg230_index = 0;
	u8 m_map_address = 0;
	u8 m_crtc_index = 0;
	u8 m_cga_mode = 0;
	u8 m_cga_mode_b = 0;
	u8 m_cga_color = 0;
	u8 m_ppi_b = 0;
	std::array<u8, 5> m_touch_packet{};
	u8 m_touch_packet_size = 0;
	u8 m_touch_packet_pos = 0;
	u8 m_touch_control = 0;
	u16 m_last_pen_x = 0xffff;
	u16 m_last_pen_y = 0xffff;
	bool m_last_pen_down = false;
	bool m_touch_irq_pending = false;
	emu_timer *m_touch_timer = nullptr;
	std::array<u8, 8> m_uart_regs{};
	u16 m_uart_divisor = 0;
	bool m_uart_thre_irq = false;
	bool m_uart_tx_empty = true;
	std::array<u8, 1024> m_uart_rx{};
	u16 m_uart_rx_head = 0;
	u16 m_uart_rx_tail = 0;
	u16 m_uart_rx_count = 0;
	u8 m_uart_irq_line = 4;
	std::array<u8, 8> m_sio1_regs{};
	u16 m_sio1_divisor = 0;
	std::array<u8, 1024> m_sio1_rx{};
	u16 m_sio1_rx_head = 0;
	u16 m_sio1_rx_tail = 0;
	u16 m_sio1_rx_count = 0;
	std::array<u8, 256> m_modem_command{};
	u16 m_modem_command_size = 0;
	bool m_modem_echo = true;
	bool m_rf_control_mode = false;
	bool m_rf_ready_announced = false;
	bool m_phone_power = false;
	bool m_phone_call = false;
	bool m_phone_ring = false;
	bool m_phone_muted = false;
	u8 m_rf_call_state = 0x64;
	bool m_answer_request_sent = false;
	bool m_call_request_sent = false;
	u8 m_rf_tx_address = 0;
	bool m_rf_dial_frame = false;
	bool m_rf_answer_prefix = false;
	bool m_rf_nam_number_pending = false;
	std::array<u8, 32> m_rf_dial_digits{};
	u8 m_rf_dial_size = 0;
	u8 m_rf_event_phase = 0;
	u8 m_rf_event_ticks = 0;
	bool m_lcd_backlight = true;
	bool m_last_incoming = false;
	u8 m_last_cellular_system = 0xff;
	u8 m_cellular_registration = 0xff;
	u8 m_cellular_signal = 6;
	std::array<u8, 10> m_cellular_number{};
	std::array<u8, 32> m_cellular_operator{};
	std::array<u8, 128> m_cellular_link_line{};
	u8 m_cellular_link_line_size = 0;
	emu_timer *m_cellular_timer = nullptr;
	emu_timer *m_uart_tx_timer = nullptr;
	emu_timer *m_uart_rx_irq_timer = nullptr;
	emu_timer *m_sio1_rx_irq_timer = nullptr;

	void mem_map(address_map &map) ATTR_COLD;
	void io_map(address_map &map) ATTR_COLD;
	u8 conventional_r(offs_t offset);
	void conventional_w(offs_t offset, u8 data);
	u8 window_r(offs_t offset);
	void window_w(offs_t offset, u8 data);
	u8 vg230_data_r();
	void vg230_data_w(u8 data);
	void vg230_index_w(u8 data);
	u8 map_data_r(offs_t offset);
	void map_data_w(offs_t offset, u8 data);
	u8 cga_r(offs_t offset);
	void cga_w(offs_t offset, u8 data);
	u8 ppi_b_r();
	void ppi_b_w(u8 data);
	u8 parallel_r(offs_t offset);
	void parallel_w(offs_t offset, u8 data);
	u8 touch_data_r();
	void touch_data_w(u8 data);
	u8 touch_control_r();
	void touch_control_w(u8 data);
	u8 uart_r(offs_t offset);
	void uart_w(offs_t offset, u8 data);
	u8 uart2_r(offs_t offset);
	void uart2_w(offs_t offset, u8 data);
	u8 uart1_r(offs_t offset);
	void uart1_w(offs_t offset, u8 data);
	void uart_update_irq();
	void uart_rx_byte(u8 data);
	void sio1_update_irq();
	void sio1_rx_byte(u8 data);
	void rf_receive_byte(u8 data);
	void modem_receive_byte(u8 data);
	void modem_execute_command();
	void modem_response(std::string_view text);
	void cellular_link_output(u8 data);
	void cellular_link_text(std::string_view text);
	void rf_handset_command(u8 command);
	void rf_schedule_event(u8 phase, u8 ticks);
	void cellular_link_command();
	void cellular_report_network();
	void cellular_start_ringing();
	TIMER_CALLBACK_MEMBER(touch_tick);
	TIMER_CALLBACK_MEMBER(cellular_tick);
	TIMER_CALLBACK_MEMBER(uart_tx_done);
	TIMER_CALLBACK_MEMBER(uart_rx_irq);
	TIMER_CALLBACK_MEMBER(sio1_rx_irq);
	u32 screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);
};

void ibmsimon_state::machine_start()
{
	save_item(NAME(m_upper_ram));
	save_item(NAME(m_video_ram));
	save_item(NAME(m_vg230_regs));
	save_item(NAME(m_map_low));
	save_item(NAME(m_map_high));
	save_item(NAME(m_crtc_regs));
	save_item(NAME(m_parallel_regs));
	save_item(NAME(m_dma_page_regs));
	save_item(NAME(m_vg230_index));
	save_item(NAME(m_map_address));
	save_item(NAME(m_crtc_index));
	save_item(NAME(m_cga_mode));
	save_item(NAME(m_cga_mode_b));
	save_item(NAME(m_cga_color));
	save_item(NAME(m_ppi_b));
	save_item(NAME(m_touch_packet));
	save_item(NAME(m_touch_packet_size));
	save_item(NAME(m_touch_packet_pos));
	save_item(NAME(m_touch_control));
	save_item(NAME(m_last_pen_x));
	save_item(NAME(m_last_pen_y));
	save_item(NAME(m_last_pen_down));
	save_item(NAME(m_touch_irq_pending));
	save_item(NAME(m_uart_regs));
	save_item(NAME(m_uart_divisor));
	save_item(NAME(m_uart_thre_irq));
	save_item(NAME(m_uart_tx_empty));
	save_item(NAME(m_uart_rx));
	save_item(NAME(m_uart_rx_head));
	save_item(NAME(m_uart_rx_tail));
	save_item(NAME(m_uart_rx_count));
	save_item(NAME(m_uart_irq_line));
	save_item(NAME(m_sio1_regs));
	save_item(NAME(m_sio1_divisor));
	save_item(NAME(m_sio1_rx));
	save_item(NAME(m_sio1_rx_head));
	save_item(NAME(m_sio1_rx_tail));
	save_item(NAME(m_sio1_rx_count));
	save_item(NAME(m_modem_command));
	save_item(NAME(m_modem_command_size));
	save_item(NAME(m_modem_echo));
	save_item(NAME(m_rf_control_mode));
	save_item(NAME(m_rf_ready_announced));
	save_item(NAME(m_phone_power));
	save_item(NAME(m_phone_call));
	save_item(NAME(m_phone_ring));
	save_item(NAME(m_phone_muted));
	save_item(NAME(m_rf_call_state));
	save_item(NAME(m_answer_request_sent));
	save_item(NAME(m_call_request_sent));
	save_item(NAME(m_rf_tx_address));
	save_item(NAME(m_rf_dial_frame));
	save_item(NAME(m_rf_answer_prefix));
	save_item(NAME(m_rf_nam_number_pending));
	save_item(NAME(m_rf_dial_digits));
	save_item(NAME(m_rf_dial_size));
	save_item(NAME(m_rf_event_phase));
	save_item(NAME(m_rf_event_ticks));
	save_item(NAME(m_lcd_backlight));
	save_item(NAME(m_last_incoming));
	save_item(NAME(m_last_cellular_system));
	save_item(NAME(m_cellular_registration));
	save_item(NAME(m_cellular_signal));
	save_item(NAME(m_cellular_number));
	save_item(NAME(m_cellular_operator));
	save_item(NAME(m_cellular_link_line));
	save_item(NAME(m_cellular_link_line_size));

	m_touch_timer = timer_alloc(FUNC(ibmsimon_state::touch_tick), this);
	m_touch_timer->adjust(attotime::from_hz(1000), 0, attotime::from_hz(1000));
	m_cellular_timer = timer_alloc(FUNC(ibmsimon_state::cellular_tick), this);
	m_cellular_timer->adjust(attotime::from_hz(20), 0, attotime::from_hz(20));
	m_uart_tx_timer = timer_alloc(FUNC(ibmsimon_state::uart_tx_done), this);
	m_uart_rx_irq_timer = timer_alloc(FUNC(ibmsimon_state::uart_rx_irq), this);
	m_sio1_rx_irq_timer = timer_alloc(FUNC(ibmsimon_state::sio1_rx_irq), this);

	// MAME hides a captured host cursor; keep its LCD-space pen pointer visible.
	auto &pointer = machine().crosshair().get_crosshair(0);
	pointer.set_mode(CROSSHAIR_VISIBILITY_ON);
	pointer.set_visible(true);
}

void ibmsimon_state::machine_reset()
{
	std::fill(m_vg230_regs.begin(), m_vg230_regs.end(), 0);
	std::fill(m_map_low.begin(), m_map_low.end(), 0);
	std::fill(m_map_high.begin(), m_map_high.end(), 0);
	std::fill(m_crtc_regs.begin(), m_crtc_regs.end(), 0);
	std::fill(m_parallel_regs.begin(), m_parallel_regs.end(), 0);
	std::fill(m_dma_page_regs.begin(), m_dma_page_regs.end(), 0);
	m_parallel_regs[1] = 0xd8; // idle Centronics status: not busy, selected, no error

	// Hardware reset defaults documented by the VG230 data manual.
	m_vg230_regs[0x01] = 0x40; // CPUOSC / 4
	m_vg230_regs[0x04] = 0x00; // 640 KiB conventional RAM, mapper disabled
	m_vg230_regs[0x08] = 0x00; // matrix keyboard mode
	m_vg230_regs[0x0a] = 0xff; // no matrix key pressed
	m_vg230_regs[0x0b] = 0xff;
	m_vg230_regs[0x0c] = 0x0f;
	// The VG230 integrated SIO is COM1/IRQ4 and connects to the Mitsubishi
	// three-wire RF deck.  The Cirrus CL-MD1224 data/fax modem is COM2/IRQ3.
	m_vg230_regs[0x10] = 0x80;
	// Simon firmware expects its internal 8-bit memory-card interface in slot 0.
	// Ready, batteries good, present, no change/timeout, writable and powered.
	m_vg230_regs[0x22] = 0xec;
	// External slot 1 is empty: ready/status inputs high, PRESENT high, power on.
	m_vg230_regs[0x28] = 0xfc;
	m_vg230_regs[0x38] = 0x28; // default top of conventional memory: 640 KiB
	m_vg230_regs[0x40] = 0xf0; // ICU shadow reserved bits
	m_vg230_regs[0xc1] = 0x01; // PMU registers start write-protected
	m_vg230_regs[0xc6] = 0xfe; // documented PMU PWRON reset value
	m_vg230_regs[0xcf] = 0x0a; // documented LCD inactivity default: 10 minutes

	m_vg230_index = 0;
	m_map_address = 0;
	m_crtc_index = 0;
	m_cga_mode = 0;
	m_cga_mode_b = 0;
	m_cga_color = 0;
	m_ppi_b = 0;
	m_mb->m_pit8253->write_gate2(0);
	m_mb->pc_speaker_set_spkrdata(0);
	m_touch_packet.fill(0);
	m_touch_packet_size = 0;
	m_touch_packet_pos = 0;
	m_touch_control = 0;
	m_last_pen_x = 0xffff;
	m_last_pen_y = 0xffff;
	m_last_pen_down = false;
	m_touch_irq_pending = false;
	m_mb->m_pic8259->ir6_w(0);
	m_uart_regs.fill(0);
	m_uart_divisor = 0;
	m_uart_thre_irq = false;
	m_uart_tx_empty = true;
	m_uart_tx_timer->adjust(attotime::never);
	m_uart_rx_irq_timer->adjust(attotime::never);
	m_uart_rx.fill(0);
	m_uart_rx_head = 0;
	m_uart_rx_tail = 0;
	m_uart_rx_count = 0;
	m_uart_irq_line = 3;
	m_sio1_regs.fill(0);
	m_sio1_divisor = 0;
	m_sio1_rx.fill(0);
	m_sio1_rx_head = 0;
	m_sio1_rx_tail = 0;
	m_sio1_rx_count = 0;
	m_sio1_rx_irq_timer->adjust(attotime::never);
	m_modem_command.fill(0);
	m_modem_command_size = 0;
	m_modem_echo = true;
	m_rf_control_mode = false;
	m_rf_ready_announced = false;
	m_phone_power = false;
	m_phone_call = false;
	m_phone_ring = false;
	m_phone_muted = false;
	m_rf_call_state = 0x64;
	m_answer_request_sent = false;
	m_call_request_sent = false;
	m_rf_tx_address = 0;
	m_rf_dial_frame = false;
	m_rf_answer_prefix = false;
	m_rf_nam_number_pending = false;
	m_rf_dial_digits.fill(0);
	m_rf_dial_size = 0;
	m_rf_event_phase = 0;
	m_rf_event_ticks = 0;
	m_lcd_backlight = false;
	m_last_incoming = false;
	m_last_cellular_system = 0xff;
	m_cellular_registration = 0xff;
	m_cellular_signal = 6;
	m_cellular_number.fill('0');
	m_cellular_operator.fill(0);
	m_cellular_link_line_size = 0;
	m_mb->m_pic8259->ir3_w(0);
	m_mb->m_pic8259->ir4_w(0);
}

void ibmsimon_state::mem_map(address_map &map)
{
	map.unmap_value_high();
	map(0x00000, 0x7ffff).rw(FUNC(ibmsimon_state::conventional_r), FUNC(ibmsimon_state::conventional_w));
	// All 32 of the upper 16 KiB pages are behind the VG230 mapper.  At reset
	// the top four pages expose the BIOS, but software may subsequently map RAM
	// over F0000-FFFFF (the BIOS remains the read source while no page mapping is
	// active).  Keeping this range as a permanent ROM prevents PHONE.EXE/GEOS
	// from saving its high-memory call context and makes its UI audit loop.
	map(0x80000, 0xfffff).rw(FUNC(ibmsimon_state::window_r), FUNC(ibmsimon_state::window_w));
}

u8 ibmsimon_state::conventional_r(offs_t offset)
{
	return m_mainram->pointer()[offset];
}

void ibmsimon_state::conventional_w(offs_t offset, u8 data)
{
	m_mainram->pointer()[offset] = data;
}

u8 ibmsimon_state::window_r(offs_t offset)
{
	const u32 address = 0x80000 + offset;

	if (address >= 0xb0000 && address <= 0xbffff)
		return m_video_ram[(address - 0xb0000) & 0x7fff];

	const unsigned page = address >> 14;
	const u8 high = m_map_high[page];
	const bool mapping_enabled = BIT(m_vg230_regs[0x04], 7) && BIT(high, 7);
	if (mapping_enabled)
	{
		const u32 physical = (u32(high & 0x0f) << 22) | (u32(m_map_low[page]) << 14) | (address & 0x3fff);
		switch ((high >> 4) & 0x07)
		{
		case 1: // system RAM
			if (physical < m_mainram->size())
				return m_mainram->pointer()[physical];
			return m_upper_ram[(physical - m_mainram->size()) & (m_upper_ram.size() - 1)];
		case 2: // ROM 0: heavy-access system BIOS ROM
			return reinterpret_cast<const u8 *>(&m_bios[0])[physical & 0x1ffff];
		case 3: // ROM 1: Simon flash / ROM-DOS disk
			return m_flash[physical & 0xfffff];
		default:
			return 0xff;
		}
	}

	// With global mapping disabled, 080000-09ffff is conventional RAM and the
	// reset window at F0000-FFFFF exposes the physical BIOS.
	if (address < 0xa0000)
		return m_upper_ram[address - 0x80000];
	if (address >= 0xf0000)
		return reinterpret_cast<const u8 *>(&m_bios[0])[address - 0xf0000];
	return 0xff;
}

void ibmsimon_state::window_w(offs_t offset, u8 data)
{
	const u32 address = 0x80000 + offset;
	if (address >= 0xb0000 && address <= 0xbffff)
	{
		m_video_ram[(address - 0xb0000) & 0x7fff] = data;
		return;
	}

	const unsigned page = address >> 14;
	const u8 high = m_map_high[page];
	if (BIT(m_vg230_regs[0x04], 7) && BIT(high, 7) && (((high >> 4) & 0x07) == 1))
	{
		const u32 physical = (u32(high & 0x0f) << 22) | (u32(m_map_low[page]) << 14) | (address & 0x3fff);
		if (physical < m_mainram->size())
			m_mainram->pointer()[physical] = data;
		else
			m_upper_ram[(physical - m_mainram->size()) & (m_upper_ram.size() - 1)] = data;
	}
	else if (address < 0xa0000)
	{
		m_upper_ram[address - 0x80000] = data;
	}
}

void ibmsimon_state::vg230_index_w(u8 data)
{
	m_vg230_index = data;
}

u8 ibmsimon_state::vg230_data_r()
{
	u8 result;
	if (m_vg230_index == 0x30 || m_vg230_index == 0x31)
	{
		const u16 timebase = u16(machine().time().as_ticks(1'193'182));
		result = (m_vg230_index == 0x30) ? u8(timebase) : u8(timebase >> 8);
	}
	else
	{
		result = m_vg230_regs[m_vg230_index];
	}
	// A read unlocks PMU writes after reset/resume.  Keep the exposed lock bit
	// faithful while allowing the following firmware write to proceed.
	if (m_vg230_index == 0xc1)
		m_vg230_regs[0xc1] &= ~u8(0x01);

	return result;
}

void ibmsimon_state::vg230_data_w(u8 data)
{
	if (m_vg230_index >= 0xc0 && m_vg230_index <= 0xdb && BIT(m_vg230_regs[0xc1], 0))
		return;
	// Card status pins are read-only.  Writes acknowledge latched status bits;
	// keep the physical presence/readiness state supplied by the machine.
	if (m_vg230_index == 0x22 || m_vg230_index == 0x28)
		return;

	m_vg230_regs[m_vg230_index] = data;
	if (m_vg230_index == 0xcf)
	{
		logerror("VG230 LCD timer set to %u minute(s)\n", data & 0x0f);
	}
}

u8 ibmsimon_state::map_data_r(offs_t offset)
{
	const unsigned page = m_map_address >> 2;
	return offset ? m_map_high[page] : m_map_low[page];
}

void ibmsimon_state::map_data_w(offs_t offset, u8 data)
{
	const unsigned page = m_map_address >> 2;
	if (offset)
		m_map_high[page] = data;
	else
		m_map_low[page] = data;
}

u8 ibmsimon_state::cga_r(offs_t offset)
{
	switch (offset & 0x0f)
	{
	case 0x01:
	case 0x03:
	case 0x05:
	case 0x07:
		return m_crtc_regs[m_crtc_index];
	case 0x0a:
		return 0x09; // display enabled, no light-pen switch
	default:
		return 0xff;
	}
}

void ibmsimon_state::cga_w(offs_t offset, u8 data)
{
	switch (offset & 0x0f)
	{
	case 0x00:
	case 0x02:
	case 0x04:
	case 0x06:
		m_crtc_index = data;
		break;
	case 0x01:
	case 0x03:
	case 0x05:
	case 0x07:
		m_crtc_regs[m_crtc_index] = data;
		break;
	case 0x08:
		m_cga_mode = data;
		break;
	case 0x09:
		m_cga_color = data;
		break;
	case 0x0e:
		m_cga_mode_b = data;
		break;
	}
}

u8 ibmsimon_state::parallel_r(offs_t offset)
{
	return m_parallel_regs[offset % 3];
}

void ibmsimon_state::parallel_w(offs_t offset, u8 data)
{
	const unsigned reg = offset % 3;
	if (reg != 1) // status is read-only
		m_parallel_regs[reg] = (reg == 2) ? (data & 0x3f) : data;
}

u8 ibmsimon_state::ppi_b_r()
{
	return m_ppi_b;
}

void ibmsimon_state::ppi_b_w(u8 data)
{
	m_ppi_b = data;
	m_mb->m_pit8253->write_gate2(BIT(data, 0));
	m_mb->pc_speaker_set_spkrdata(BIT(data, 1));
}

void ibmsimon_state::uart_update_irq()
{
	const bool irq = (m_uart_rx_count && BIT(m_uart_regs[1], 0)) || (m_uart_thre_irq && BIT(m_uart_regs[1], 1));
	m_mb->m_pic8259->ir3_w(irq);
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::uart_tx_done)
{
	m_uart_tx_empty = true;
	m_uart_thre_irq = true;
	uart_update_irq();
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::uart_rx_irq)
{
	// The XT PIC is edge triggered.  Present each received character on a new
	// edge after its serial character time, never while the preceding ISR is
	// still active.
	m_mb->m_pic8259->ir3_w(0);
	uart_update_irq();
}

void ibmsimon_state::uart_rx_byte(u8 data)
{
	if (m_uart_rx_count >= m_uart_rx.size())
		return;
	const bool was_empty = !m_uart_rx_count;
	m_uart_rx[m_uart_rx_tail] = data;
	m_uart_rx_tail = (m_uart_rx_tail + 1) % m_uart_rx.size();
	++m_uart_rx_count;
	if (was_empty)
		m_uart_rx_irq_timer->adjust(attotime::from_ticks(std::max<u16>(m_uart_divisor, 1) * 160, 1'843'200));
}

void ibmsimon_state::sio1_update_irq()
{
	const bool irq = m_sio1_rx_count && BIT(m_sio1_regs[1], 0);
	m_mb->m_pic8259->ir4_w(irq);
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::sio1_rx_irq)
{
	// COM1 is connected to the RF deck.  Keep consecutive three-wire status
	// bytes as separate edges for the XT PIC and the BIOS ring buffer ISR.
	m_mb->m_pic8259->ir4_w(0);
	sio1_update_irq();
}

void ibmsimon_state::sio1_rx_byte(u8 data)
{
	if (m_phone_ring || m_phone_call)
		logerror("Simon RF receive %02X ring=%d call=%d state=%02X phase=%u ticks=%u queue=%u\n",
			data, m_phone_ring, m_phone_call, m_rf_call_state, m_rf_event_phase, m_rf_event_ticks, m_sio1_rx_count);
	if (m_sio1_rx_count >= m_sio1_rx.size())
		return;
	const bool was_empty = !m_sio1_rx_count;
	m_sio1_rx[m_sio1_rx_tail] = data;
	m_sio1_rx_tail = (m_sio1_rx_tail + 1) % m_sio1_rx.size();
	++m_sio1_rx_count;
	if (was_empty)
		m_sio1_rx_irq_timer->adjust(attotime::from_ticks(std::max<u16>(m_sio1_divisor, 1) * 160, 1'843'200));
}

void ibmsimon_state::modem_response(std::string_view text)
{
	for (const char ch : text)
		uart_rx_byte(u8(ch));
}

void ibmsimon_state::cellular_link_output(u8 data)
{
	static constexpr char hex[] = "0123456789ABCDEF";
	const u8 message[] = { 'T', ' ', u8(hex[data >> 4]), u8(hex[data & 0x0f]), '\n' };
	for (u8 ch : message)
		m_cellular_link->output(ch);
}

void ibmsimon_state::cellular_link_text(std::string_view text)
{
	for (char ch : text)
		m_cellular_link->output(u8(ch));
}

void ibmsimon_state::rf_schedule_event(u8 phase, u8 ticks)
{
	if (m_rf_event_phase && m_rf_event_phase != phase)
		logerror("Simon RF event replace phase=%u ticks=%u -> phase=%u ticks=%u ring=%d call=%d originate=%d\n",
			m_rf_event_phase, m_rf_event_ticks, phase, ticks, m_phone_ring, m_phone_call, m_call_request_sent);
	m_rf_event_phase = phase;
	m_rf_event_ticks = ticks;
}

void ibmsimon_state::rf_handset_command(u8 command)
{
	// Motorola handset key codes used by Simon's three-wire BIOS.  The early
	// demo uses 60xx words, while the production PHONE.EXE captured from this
	// ROM uses 70xx.  Parsing the complete addressed word avoids mistaking
	// ordinary payload bytes for handset commands.
	char digit = 0;
	switch (command)
	{
	case 0x01: digit = '1'; break;
	case 0x02: digit = '2'; break;
	case 0x03: digit = '3'; break;
	case 0x05: digit = '4'; break;
	case 0x06: digit = '5'; break;
	case 0x07: digit = '6'; break;
	case 0x09: digit = '7'; break;
	case 0x0a: digit = '8'; break;
	case 0x0b: digit = '9'; break;
	case 0x0d: digit = '*'; break;
	case 0x0e: digit = '0'; break;
	case 0x0f: digit = '#'; break;
	default: break;
	}

	if (digit)
	{
		if (!m_call_request_sent && !m_phone_call && !m_phone_ring && m_rf_dial_size < m_rf_dial_digits.size())
			m_rf_dial_digits[m_rf_dial_size++] = u8(digit);
		return;
	}

	if (command == 0x13) // SEND (mobile-originated call)
	{
		// PHONE.EXE also emits SEND after the switch has assigned the voice
		// channel.  At that point it is an off-hook/voice-path keepalive, not a
		// second originate request.  The RF deck confirms that it remains in
		// Conversation; ignoring this word makes PHONE.EXE tear down and rebuild
		// its in-call window on every audit cycle.
		if (m_phone_call)
		{
			// This is a handset key word, not a state query.  Conversation remains
			// 6Bh; PHONE.EXE verifies it separately with the ADh/AEh audit below.
			return;
		}

		if (m_rf_dial_size && !m_phone_call && !m_phone_ring)
		{
			std::string message("O ");
			message.append(reinterpret_cast<const char *>(m_rf_dial_digits.data()), m_rf_dial_size);
			message.push_back('\n');
			cellular_link_text(message);
			m_call_request_sent = true;
			rf_schedule_event(3, 2); // originating
		}
		return;
	}

	if (command == 0x17) // END
	{
		const bool active = m_phone_ring || m_phone_call || m_call_request_sent;
		m_phone_ring = false;
		m_phone_call = false;
		m_phone_muted = false;
		m_rf_call_state = 0x64;
		m_answer_request_sent = false;
		m_rf_answer_prefix = false;
		m_call_request_sent = false;
		m_rf_dial_size = 0;
		rf_schedule_event(5, 1); // idle
		if (active)
			cellular_link_text("H\n");
	}
}

void ibmsimon_state::cellular_report_network()
{
	const u8 registration = (m_cellular_registration == 0xff)
		? (m_cellular_system->read() & 0x07)
		: m_cellular_registration;
	if (registration > 5)
	{
		sio1_rx_byte(0x82); // not registered / no service
		return;
	}

	sio1_rx_byte(0x83); // registered and in service
	rf_schedule_event(7, 4); // system class follows after 200 ms
}

void ibmsimon_state::cellular_start_ringing()
{
	if (!m_phone_power)
	{
		popmessage("Virtual 1G call not delivered: Simon phone is off");
		return;
	}
	m_phone_call = false;
	m_phone_ring = true;
	m_phone_muted = false;
	m_rf_call_state = 0x6a;
	m_answer_request_sent = false;
	m_rf_answer_prefix = false;
	// An AMPS mobile is first paged, responds to the page, and then waits for
	// the user to answer.  Preserve the real time between transitions: sending
	// all three bytes back-to-back lets PHONE.EXE miss the paging state and can
	// leave its command loop waiting indefinitely.
	sio1_rx_byte(0x63); // paging channel
	rf_schedule_event(1, 5); // page response after 250 ms
}

void ibmsimon_state::cellular_link_command()
{
	if (m_cellular_link_line_size && m_cellular_link_line[m_cellular_link_line_size - 1] == '\r')
		--m_cellular_link_line_size;
	if (!m_cellular_link_line_size)
		return;

	const u8 command = std::toupper(m_cellular_link_line[0]);
	if (command == 'R')
	{
		if (m_phone_power)
		{
			cellular_start_ringing();
			std::string caller("unknown");
			if (m_cellular_link_line_size > 2 && m_cellular_link_line[1] == ' ')
				caller.assign(reinterpret_cast<const char *>(&m_cellular_link_line[2]), m_cellular_link_line_size - 2);
			popmessage("Virtual 1G incoming call from %s\nRF state: Wait For Answer (6A)", caller.c_str());
		}
	}
	else if (command == 'C')
	{
		logerror("Simon switch C received ring=%d call=%d phase=%u ticks=%u\n",
			m_phone_ring, m_phone_call, m_rf_event_phase, m_rf_event_ticks);
		m_phone_ring = false;
		// Latch the established state as soon as the switch confirms the call.
		// The 6Bh byte is intentionally delayed to give the preceding Answer/
		// originate command time to leave the UART, but that delay must not look
		// idle to the board-control handler.  PHONE.EXE pulses the RF reset/audio
		// latch while accepting a call; with m_phone_call false that pulse replaced
		// this pending Conversation event with a registration scan ending in 64h,
		// producing the visible hourglass/no-service/in-call loop.
		m_phone_call = true;
		m_phone_muted = false;
		m_rf_call_state = 0x6b;
		m_answer_request_sent = false;
		m_rf_answer_prefix = false;
		m_call_request_sent = false;
		m_rf_event_phase = 0;
		m_rf_dial_size = 0;
		// Conversation (6Bh) clears both the RF-deck incoming latch and the
		// BIOS incoming-call flag.  Do not precede it with A8h: that byte also
		// raises BIOS event 0400h, which PHONE.EXE treats as a missed call.
		rf_schedule_event(4, 2); // conversation after the serial response gap
		popmessage("Virtual 1G call connected\nRF state: Conversation (6B)");
	}
	else if (command == 'H')
	{
		const bool was_ringing = m_phone_ring;
		const bool was_call = m_phone_call;
		m_phone_ring = false;
		m_phone_call = false;
		m_phone_muted = false;
		m_rf_call_state = 0x64;
		m_answer_request_sent = false;
		m_rf_answer_prefix = false;
		m_call_request_sent = false;
		m_rf_event_phase = 0;
		m_rf_dial_size = 0;
		if (was_ringing)
			sio1_rx_byte(0xa8); // incoming-call indication off
		if (was_call)
			sio1_rx_byte(0x81); // handset/voice channel no longer in use
		sio1_rx_byte(0x64); // idle
		popmessage("Virtual 1G call ended\nRF state: Idle (64)");
	}
	else if (command == 'S')
	{
		// Host switch profile: S <HOME1|HOME2|HOME3|HOME4|ROAM|ALTROAM|OFFLINE>
		// <0..6> [operator name].  AMPS exposes the system class and signal to
		// Simon; the free-form operator name remains available to the host link.
		std::string line(reinterpret_cast<const char *>(m_cellular_link_line.data()), m_cellular_link_line_size);
		const std::size_t first = line.find(' ');
		const std::size_t second = first == std::string::npos ? first : line.find(' ', first + 1);
		const std::size_t third = second == std::string::npos ? second : line.find(' ', second + 1);
		std::string system("UNKNOWN");
		if (first != std::string::npos)
		{
			system = line.substr(first + 1, second - first - 1);
			std::transform(system.begin(), system.end(), system.begin(), [](unsigned char ch) { return std::toupper(ch); });
			if (system == "HOME1") m_cellular_registration = 0;
			else if (system == "HOME2") m_cellular_registration = 1;
			else if (system == "HOME3") m_cellular_registration = 2;
			else if (system == "HOME4") m_cellular_registration = 3;
			else if (system == "ROAM") m_cellular_registration = 4;
			else if (system == "ALTROAM") m_cellular_registration = 5;
			else if (system == "OFFLINE" || system == "NOSERVICE") m_cellular_registration = 0xfe;
		}
		if (second != std::string::npos && second + 1 < line.size() && std::isdigit(u8(line[second + 1])))
			m_cellular_signal = std::min<u8>(line[second + 1] - '0', 6);
		m_cellular_operator.fill(0);
		if (third != std::string::npos)
		{
			const std::string name = line.substr(third + 1);
			std::copy_n(name.begin(), std::min(name.size(), m_cellular_operator.size() - 1), m_cellular_operator.begin());
		}
		if (m_phone_power)
		{
			cellular_report_network();
			if (m_rf_event_phase == 6)
				m_rf_event_phase = 0;
		}
		std::string acknowledgement("P ");
		acknowledgement.append(system);
		acknowledgement.push_back(' ');
		acknowledgement.append(std::to_string(m_cellular_signal));
		if (m_cellular_operator[0])
		{
			acknowledgement.push_back(' ');
			acknowledgement.append(reinterpret_cast<const char *>(m_cellular_operator.data()));
		}
		acknowledgement.push_back('\n');
		cellular_link_text(acknowledgement);
		popmessage("Virtual 1G profile: %s\nSignal: %u/6  Operator: %s", system.c_str(), m_cellular_signal,
			reinterpret_cast<const char *>(m_cellular_operator.data()));
	}
	else if (command == 'V')
	{
		// Virtual MIN supplied by the host switch.  The RF deck exposes a ten
		// digit MIN/NAM field; shorter laboratory extension numbers are padded on
		// the left, just like a programmed handset number.
		m_cellular_number.fill('0');
		std::string number;
		if (m_cellular_link_line_size > 2 && m_cellular_link_line[1] == ' ')
			number.assign(reinterpret_cast<const char *>(&m_cellular_link_line[2]), m_cellular_link_line_size - 2);
		number.erase(std::remove_if(number.begin(), number.end(), [](unsigned char ch) { return !std::isdigit(ch); }), number.end());
		const std::size_t count = std::min(number.size(), m_cellular_number.size());
		std::copy_n(number.end() - count, count, m_cellular_number.end() - count);
	}
	else if (command == 'I' && m_cellular_link_line_size >= 4)
	{
		auto const nibble = [] (u8 ch) -> int
		{
			ch = std::toupper(ch);
			return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
		};
		const int high = nibble(m_cellular_link_line[2]);
		const int low = nibble(m_cellular_link_line[3]);
		if (high >= 0 && low >= 0)
			sio1_rx_byte(u8((high << 4) | low));
	}
}

void ibmsimon_state::modem_execute_command()
{
	std::string command(reinterpret_cast<const char *>(m_modem_command.data()), m_modem_command_size);
	m_modem_command_size = 0;
	std::transform(command.begin(), command.end(), command.begin(), [](unsigned char ch) { return std::toupper(ch); });

	if (!command.starts_with("AT"))
	{
		modem_response("\r\nERROR\r\n");
		return;
	}

	if (command == "ATZ")
	{
		m_modem_echo = true;
		m_phone_call = false;
		m_call_request_sent = false;
	}
	else if (command.find("E0") != std::string::npos)
	{
		m_modem_echo = false;
	}
	else if (command.find("E1") != std::string::npos)
	{
		m_modem_echo = true;
	}

	if (command.find("#VCL=1") != std::string::npos)
	{
		m_phone_power = true;
	}

	if (command.starts_with("ATD") || command.find("DT") != std::string::npos)
	{
		m_phone_power = true;
		m_phone_call = false;
		m_phone_ring = false;
		m_call_request_sent = true;
		const std::size_t dial = command.starts_with("ATD") ? 3 : command.find("DT") + 2;
		std::string message("O ");
		for (std::size_t i = dial; i < command.size(); ++i)
		{
			const char ch = command[i];
			if ((ch < '0' || ch > '9') && ch != '*' && ch != '#')
				break;
			message.push_back(ch);
		}
		if (message.size() > 2)
		{
			message.push_back('\n');
			cellular_link_text(message);
		}
	}
	else if (command == "ATH" || command.starts_with("ATH0"))
	{
		m_phone_call = false;
		m_phone_ring = false;
		m_call_request_sent = false;
	}
	else if (command.find("#VCL=0") != std::string::npos)
	{
		m_phone_power = false;
		m_phone_call = false;
		m_call_request_sent = false;
	}

	// The RF deck uses a Hayes-compatible command channel with Cirrus Logic
	// voice/cellular extensions.  Unsupported setters are deliberately
	// accepted here so the command log can drive progressively deeper HLE.
	modem_response("\r\nOK\r\n");
}

void ibmsimon_state::modem_receive_byte(u8 data)
{
	if (m_modem_echo)
		uart_rx_byte(data);

	if (data == '\r')
	{
		if (m_modem_command_size)
			modem_execute_command();
		return;
	}
	if (data == '\n')
		return;
	if (data == 0x08)
	{
		if (m_modem_command_size)
			--m_modem_command_size;
		return;
	}
	if (m_modem_command_size < m_modem_command.size() - 1)
		m_modem_command[m_modem_command_size++] = data;
}

void ibmsimon_state::rf_receive_byte(u8 data)
{
	// The production hardware has two independent serial devices.  COM1 is
	// the Mitsubishi RF deck's three-wire control link; COM2 is the Cirrus
	// data/fax modem.  Keep the raw deck traffic visible to the host switch.
	cellular_link_output(data);
	if (m_phone_ring || m_phone_call)
		logerror("Simon RF transmit %02X ring=%d call=%d phase=%u ticks=%u\n",
			data, m_phone_ring, m_phone_call, m_rf_event_phase, m_rf_event_ticks);

	// Production PHONE.EXE rejects an incoming call and ends an established
	// call through INT 15h/A0h, AL=03h.  The BIOS serializes that operation as
	// the single RF-deck command 88h rather than the keypad-style 70h,17h word.
	if (data == 0x88 && (m_phone_ring || m_phone_call || m_call_request_sent))
	{
		const bool was_ringing = m_phone_ring;
		const bool was_call = m_phone_call;
		m_phone_ring = false;
		m_phone_call = false;
		m_phone_muted = false;
		m_rf_call_state = 0x64;
		m_answer_request_sent = false;
		m_rf_answer_prefix = false;
		m_call_request_sent = false;
		m_rf_dial_size = 0;
		m_rf_event_phase = 0;
		// PHONE.EXE's release routine waits for the deck ACK before it tears
		// down the call window.  Omitting it leaves the UI in its hourglass
		// retry loop even though the host switch has already hung up.
		sio1_rx_byte(0x21);
		if (was_ringing)
			sio1_rx_byte(0xa8); // clear the incoming-call indication
		if (was_call)
			sio1_rx_byte(0x81); // clear handset/voice-channel-in-use status
		sio1_rx_byte(0x64); // idle
		cellular_link_text("H\n");
		return;
	}

	// 89h,1bh terminates PHONE.EXE's dial-string frame.  It is deliberately
	// also the native Answer request while the deck is in Wait For Answer.  The
	// call-state context disambiguates it from normal mobile-originated dialing.
	if (m_phone_ring && data == 0x89)
	{
		m_rf_answer_prefix = true;
		return;
	}
	if (m_rf_answer_prefix)
	{
		m_rf_answer_prefix = false;
		if (m_phone_ring && data == 0x1b)
		{
			if (!m_answer_request_sent)
			{
				m_answer_request_sent = true;
				m_rf_event_phase = 0;
				cellular_link_text("A\n");
			}
			return;
		}
	}

	// Production PHONE.EXE sends the dial string as:
	//   18h, ASCII digits, 89h, 1bh
	// followed by the handset command 70h,13h (SEND).  Preserve the completed
	// number until SEND arrives; the 1bh trailer is framing, not phone power.
	if (data == 0x18)
	{
		m_rf_dial_frame = true;
		m_rf_dial_size = 0;
		return;
	}
	if (m_rf_dial_frame)
	{
		if ((data >= '0' && data <= '9') || data == '*' || data == '#')
		{
			if (m_rf_dial_size < m_rf_dial_digits.size())
				m_rf_dial_digits[m_rf_dial_size++] = data;
			return;
		}
		if (data == 0x89 || data == 0x1b)
		{
			m_rf_dial_frame = false;
			return;
		}
		m_rf_dial_frame = false;
	}

	// RF-deck configuration queries used during startup and after releasing a
	// call.  PHONE.EXE waits as long as three seconds for each response, so a
	// silent HLE deck causes the visible hourglass/no-service retry cycle.
	if (data == 0xa4)
	{
		sio1_rx_byte(0x79); // valid/current NAM configuration
		return;
	}
	if (data == 0xa2)
	{
		const u8 registration = (m_cellular_registration == 0xff)
			? (m_cellular_system->read() & 0x07)
			: m_cellular_registration;
		sio1_rx_byte(0x71 + std::min<u8>(registration, 3));
		return;
	}

	// PHONE.EXE follows the active-NAM query with 71h..74h probes.  A present
	// NAM echoes its selector; A9h then requests the associated ten-byte MIN.
	// Omitting this exchange makes PHONE.EXE time out, restart its RF audit and
	// redraw the Phone screen forever even though Conversation (6Bh) arrived.
	if (!m_rf_tx_address && data >= 0x71 && data <= 0x74)
	{
		m_rf_nam_number_pending = true;
		sio1_rx_byte(data);
		return;
	}
	if (m_rf_nam_number_pending && data == 0xa9)
	{
		m_rf_nam_number_pending = false;
		sio1_rx_byte(0xa9);
		for (u8 digit : m_cellular_number)
			sio1_rx_byte(digit);
		return;
	}
	if (m_rf_nam_number_pending)
		m_rf_nam_number_pending = false;

	// ADh is the production RF deck's call-state audit.  PHONE.EXE does not
	// merely consult the BIOS status block: after entering (and periodically
	// while maintaining) a call it transmits ADh and waits for the deck to
	// restate its live 64h/6Ah/6Bh state.  Leaving this request unanswered made
	// the application alternate between its idle and in-call windows until its
	// synchronous retries eventually wedged the UI, even though the original
	// Conversation indication remained correctly latched in the BIOS.
	if (data == 0xad)
	{
		sio1_rx_byte(m_phone_call || m_phone_ring ? m_rf_call_state : 0x64);
		return;
	}

	// PHONE.EXE's call-entry audit is explicitly two-phase.  It first sends ADh
	// and consumes the live 6Bh state, waits 500 ms, then sends AEh.  On its next
	// pass it waits up to 200 ms for an AEh acknowledgement before committing the
	// in-call window.  Suppressing this acknowledgement makes every audit time
	// out, which is the visible idle/in-call redraw loop.
	if (data == 0xae && m_phone_call)
	{
		sio1_rx_byte(0xae);
		return;
	}

	// Once the AMPS voice channel enters Conversation, PHONE.EXE asks the RF
	// deck for the current audio path with 0dh and waits for 0eh/0fh.  PHONE.EXE
	// treats 0eh as the muted state (it sets its Phone Muted flag and changes
	// the button to Unmute); 0fh is the normal, unmuted voice path.  Leaving the
	// query unanswered makes the application time out and re-audit the deck.
	if (!m_rf_tx_address && data == 0x0d && m_phone_call)
	{
		sio1_rx_byte(m_phone_muted ? 0x0e : 0x0f);
		return;
	}

	// PHONE.EXE uses separate unaddressed latch commands: 0Eh mutes and 0Fh
	// restores the voice path, then audits the result with 0Dh.  Both bytes can
	// also be handset keypad commands when preceded by an address, so only
	// consume them with no pending address.
	if (!m_rf_tx_address && data == 0x0e && m_phone_call)
	{
		m_phone_muted = true;
		return;
	}

	if (!m_rf_tx_address && data == 0x0f && m_phone_call)
	{
		m_phone_muted = false;
		return;
	}

	// The RF deck POST and AUDIT command both acknowledge with 21h.  Normal
	// keypad/status traffic is asynchronous and must not be echoed byte-for-byte.
	if (data == 0x24)
	{
		sio1_rx_byte(0x21);
		return;
	}

	// INT 15h/AH=98h serializes the handset word as address then command.
	// Peripheral commands use a0h and are deliberately kept visible but are
	// not interpreted as handset keys.
	if (data == 0x60 || data == 0x70 || data == 0xa0)
	{
		m_rf_tx_address = data;
		return;
	}

	if (m_rf_tx_address)
	{
		const u8 address = m_rf_tx_address;
		m_rf_tx_address = 0;
		if ((address == 0x60 || address == 0x70) && data != 0x7f && data != 0x6b)
			rf_handset_command(data);
	}
}

u8 ibmsimon_state::uart_r(offs_t offset)
{
	offset &= 7;
	if (BIT(m_uart_regs[3], 7))
	{
		if (offset == 0)
			return u8(m_uart_divisor);
		if (offset == 1)
			return u8(m_uart_divisor >> 8);
	}

	switch (offset)
	{
	case 0:
		if (m_uart_rx_count)
		{
			const u8 data = m_uart_rx[m_uart_rx_head];
			m_uart_rx_head = (m_uart_rx_head + 1) % m_uart_rx.size();
			--m_uart_rx_count;
			// COM2 is the data/fax modem on IRQ3.  It must never clear COM1's
			// independent RF-deck IRQ4: doing that loses incoming-call and call-
			// state notification edges while leaving the BIOS status half-updated.
			m_mb->m_pic8259->ir3_w(0);
			if (m_uart_rx_count)
				m_uart_rx_irq_timer->adjust(attotime::from_ticks(std::max<u16>(m_uart_divisor, 1) * 160, 1'843'200));
			else
				uart_update_irq();
			return data;
		}
		return 0xff;
	case 1:
		return m_uart_regs[1];
	case 2:
		if (m_uart_rx_count && BIT(m_uart_regs[1], 0))
			return 0x04;
		if (m_uart_thre_irq && BIT(m_uart_regs[1], 1))
		{
			m_uart_thre_irq = false;
			uart_update_irq();
			return 0x02;
		}
		return 0x01;
	case 5:
		return (m_uart_tx_empty ? 0x60 : 0x00) | (m_uart_rx_count ? 0x01 : 0x00);
	case 6:
		// COM2 is the separate data/fax modem.  Keep its idle handshake asserted;
		// cellular RI/DCD belong to the RF deck on COM1, not this UART.
		return 0x30;
	default:
		return m_uart_regs[offset];
	}
}

u8 ibmsimon_state::uart2_r(offs_t offset)
{
	m_uart_irq_line = 3;
	return uart_r(offset);
}

void ibmsimon_state::uart2_w(offs_t offset, u8 data)
{
	m_uart_irq_line = 3;
	uart_w(offset, data);
}

u8 ibmsimon_state::uart1_r(offs_t offset)
{
	offset &= 7;
	if (BIT(m_sio1_regs[3], 7))
	{
		if (offset == 0) return u8(m_sio1_divisor);
		if (offset == 1) return u8(m_sio1_divisor >> 8);
	}
	if (offset == 0)
	{
		if (!m_sio1_rx_count)
			return 0xff;
		const u8 data = m_sio1_rx[m_sio1_rx_head];
		m_sio1_rx_head = (m_sio1_rx_head + 1) % m_sio1_rx.size();
		--m_sio1_rx_count;
		m_mb->m_pic8259->ir4_w(0);
		if (m_sio1_rx_count)
			m_sio1_rx_irq_timer->adjust(attotime::from_ticks(std::max<u16>(m_sio1_divisor, 1) * 160, 1'843'200));
		else
			sio1_update_irq();
		return data;
	}
	if (offset == 2) return (m_sio1_rx_count && BIT(m_sio1_regs[1], 0)) ? 0x04 : 0x01;
	if (offset == 5) return 0x60 | (m_sio1_rx_count ? 0x01 : 0x00);
	if (offset == 6)
	{
		// COM1 is the three-wire RF deck used by PHONE.EXE.  In addition to the
		// asynchronous RF state bytes, the real deck presents RI while paging and
		// holds DCD for the lifetime of an established call.  Supplying these on
		// COM2 made Conversation (6Bh) transient: PHONE.EXE immediately saw the
		// carrier disappear and restarted its service/acquisition state machine.
		return 0x30 | (m_phone_ring ? 0x40 : 0x00) | (m_phone_call ? 0x80 : 0x00);
	}
	return m_sio1_regs[offset];
}

void ibmsimon_state::uart1_w(offs_t offset, u8 data)
{
	offset &= 7;
	if (BIT(m_sio1_regs[3], 7))
	{
		if (offset == 0)
		{
			m_sio1_divisor = (m_sio1_divisor & 0xff00) | data;
			return;
		}
		if (offset == 1)
		{
			m_sio1_divisor = (m_sio1_divisor & 0x00ff) | (u16(data) << 8);
			return;
		}
	}
	if (offset == 0)
	{
		rf_receive_byte(data);
	}
	else if (offset == 1)
	{
		m_sio1_regs[offset] = data & 0x0f;
		sio1_update_irq();
	}
	else if (offset != 2 && offset != 5 && offset != 6)
	{
		m_sio1_regs[offset] = data;
	}
}

void ibmsimon_state::uart_w(offs_t offset, u8 data)
{
	offset &= 7;
	if (BIT(m_uart_regs[3], 7))
	{
		if (offset == 0)
		{
			m_uart_divisor = (m_uart_divisor & 0xff00) | data;
			return;
		}
		if (offset == 1)
		{
			m_uart_divisor = (m_uart_divisor & 0x00ff) | (u16(data) << 8);
			return;
		}
	}

	switch (offset)
	{
	case 0:
		m_uart_thre_irq = false;
		m_uart_tx_empty = false;
		if (BIT(m_uart_regs[4], 4))
			uart_rx_byte(data);
		else
			modem_receive_byte(data);
		// The integrated SIO uses the standard 1.8432 MHz UART clock.  Model
		// one start bit, eight data bits and one stop bit before THRE rises.
		m_uart_tx_timer->adjust(attotime::from_ticks(std::max<u16>(m_uart_divisor, 1) * 160, 1'843'200));
		uart_update_irq();
		break;
	case 1:
		m_uart_regs[1] = data & 0x0f;
		if (BIT(m_uart_regs[1], 1) && m_uart_tx_empty)
			m_uart_thre_irq = true;
		uart_update_irq();
		break;
	case 2:
		// 8250 has no FIFOs; a write is accepted for 16450-compatible probes.
		break;
	case 3:
	case 7:
		m_uart_regs[offset] = data;
		break;
	case 4:
		m_uart_regs[offset] = data;
		break;
	default:
		break;
	}
}

u8 ibmsimon_state::touch_data_r()
{
	if (m_touch_irq_pending)
	{
		m_mb->m_pic8259->ir6_w(0);
		m_touch_irq_pending = false;
	}

	if (m_touch_packet_pos < m_touch_packet_size)
		return m_touch_packet[m_touch_packet_pos++];
	return 0xff;
}

void ibmsimon_state::touch_data_w(u8 data)
{
	// Bit 6 is pulsed by the BIOS when it resets/calibrates the digitizer.
	// Coordinate reports are generated from the emulated pen inputs below.
	// This port is also the Simon board's analogue/audio routing latch.  Keep
	// writes observable while ringing, but do not infer call answer from them:
	// PHONE.EXE also selects route 0 while leaving the phone screen.
	if (m_phone_ring)
		logerror("Simon audio route write %02X while ringing\n", data);
}

u8 ibmsimon_state::touch_control_r()
{
	return m_touch_control;
}

void ibmsimon_state::touch_control_w(u8 data)
{
	const u8 previous = m_touch_control;
	m_touch_control = data;
	// The production inverter inhibit is bit 0 of this shared board latch.
	// Simon BIOS implements its own one-minute timeout and clears this bit;
	// pen/keyboard activity sets it again.  CFh is deliberately disabled.
	m_lcd_backlight = BIT(data, 0);
	if (BIT(previous, 0) != BIT(data, 0))
		logerror("Simon LCD backlight %s at %.3f s\n", m_lcd_backlight ? "on" : "off", machine().time().as_double());

	// Port 0172h is a shared Simon board-control latch, not solely the pen
	// controller.  BIOS initialization pulses bit 3 to reset/power-test the
	// Mitsubishi RF deck.  A healthy deck finishes POST with status 21h.
	if (BIT(previous, 3) && !BIT(data, 3))
	{
		logerror("Simon RF reset pulse ring=%d call=%d originate=%d phase=%u ticks=%u\n",
			m_phone_ring, m_phone_call, m_call_request_sent, m_rf_event_phase, m_rf_event_ticks);
		// Every RF power/reset test requires a fresh 21h POST response.  This is
		// separate from the host-switch registration handshake below: suppressing
		// repeated N packets must never suppress the hardware acknowledgement.
		sio1_rx_byte(0x21);
		m_rf_control_mode = true;
		m_phone_power = true;
		if (!m_rf_ready_announced)
		{
			// Tell a connected virtual switch that the RF deck can now consume its
			// registration profile.  The switch may have accepted the socket before
			// BIOS POST, so its initial S line is not sufficient as a handshake.
			m_rf_ready_announced = true;
			cellular_link_text("N\n");
		}
		// PHONE.EXE clears the BIOS cellular status while starting the phone.
		// Re-report registration after an idle power-test burst, but never replace
		// an in-progress page/alert sequence with this background status event.
		if (!m_phone_ring && !m_phone_call && !m_call_request_sent)
			rf_schedule_event(6, 20);
	}
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::touch_tick)
{
	// The BIOS consumes one byte per IRQ6.  Do not raise the next interrupt
	// until the previous byte has been read from port 0170h.
	if (m_touch_irq_pending)
		return;

	if (m_touch_packet_pos >= m_touch_packet_size)
	{
		const bool down = BIT(m_pen_button->read(), 0);
		const u16 x = std::min<u16>(m_pen_x->read(), 639);
		const u16 y = std::min<u16>(m_pen_y->read(), 199);

		if (down)
		{
			// Simon uses a five-byte absolute packet.  The two 10-bit values
			// are calibrated by the BIOS to 640x200 before INT 33h sees them.
			const u16 raw_x = 31 + ((u32(x) * (977 - 31) + 319) / 639);
			const u16 raw_y = 118 + ((u32(y) * (885 - 118) + 99) / 199);
			m_touch_packet = { 0xff, u8(raw_x >> 8), u8(raw_x), u8(raw_y >> 8), u8(raw_y) };
			m_touch_packet_size = 5;
			m_touch_packet_pos = 0;
			m_last_pen_x = x;
			m_last_pen_y = y;
			m_last_pen_down = true;
		}
		else if (m_last_pen_down)
		{
			// FF FE FE is the pen-up sequence recognized by the Simon BIOS.
			m_touch_packet = { 0xff, 0xfe, 0xfe, 0x00, 0x00 };
			m_touch_packet_size = 3;
			m_touch_packet_pos = 0;
			m_last_pen_down = false;
		}
		else
		{
			return;
		}
	}

	m_mb->m_pic8259->ir6_w(1);
	m_touch_irq_pending = true;
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::cellular_tick)
{
	u8 input[64];
	const u32 received = m_cellular_link->input(input, sizeof(input));
	for (u32 i = 0; i < received; ++i)
	{
		if (input[i] == '\n')
		{
			cellular_link_command();
			m_cellular_link_line_size = 0;
		}
		else if (m_cellular_link_line_size < m_cellular_link_line.size())
		{
			m_cellular_link_line[m_cellular_link_line_size++] = input[i];
		}
	}

	if (m_rf_event_phase && (!m_rf_event_ticks || !--m_rf_event_ticks))
	{
		switch (m_rf_event_phase)
		{
		case 1:
			sio1_rx_byte(0x66); // page response
			rf_schedule_event(10, 6); // voice-channel order response
			break;
		case 2:
			sio1_rx_byte(0x6a); // wait for answer / ring
			rf_schedule_event(12, 1); // handset alert indication on the next IRQ-safe tick
			break;
		case 3:
			sio1_rx_byte(0x65); // originating
			m_rf_event_phase = 0;
			break;
		case 4:
			m_phone_call = true;
			m_phone_muted = false;
			logerror("Simon RF emit Conversation 6B + Off-hook 80 ring=%d call=%d\n", m_phone_ring, m_phone_call);
			sio1_rx_byte(0x6b); // conversation
			// The 60h-6fh state and the handset/voice-channel status are separate
			// signals.  BIOS byte 80h sets status-block offset 2 bit 0; PHONE.EXE
			// uses that bit to select Phone In Use, run the airtime timer and enable
			// in-call controls.  81h clears it on release.
			sio1_rx_byte(0x80); // handset off hook / voice channel in use
			m_rf_event_phase = 0;
			break;
		case 5:
			sio1_rx_byte(0x81); // handset/voice channel no longer in use
			sio1_rx_byte(0x64); // idle
			m_rf_event_phase = 0;
			break;
		case 6:
			cellular_report_network();
			break;
		case 7:
		{
			const u8 registration = (m_cellular_registration == 0xff)
				? (m_cellular_system->read() & 0x07)
				: m_cellular_registration;
			if (registration <= 5)
				sio1_rx_byte(0x84 + registration); // Home 1-4/Roam
			rf_schedule_event(8, 4);
			break;
		}
		case 8:
			sio1_rx_byte(0xa0 + std::min<u8>(m_cellular_signal, 6));
			rf_schedule_event(9, 4);
			break;
		case 9:
			sio1_rx_byte(0x64); // registered and idle
			m_rf_event_phase = 0;
			break;
		case 10:
			sio1_rx_byte(0x67); // order response / voice channel assignment
			rf_schedule_event(11, 6);
			break;
		case 11:
			sio1_rx_byte(0x6e); // SAT acquired on the assigned voice channel
			rf_schedule_event(2, 6); // alert order, then wait for user answer
			break;
		case 12:
			// The 60h-6fh values only update the BIOS call-state byte.  A7h is
			// the RF deck's separate incoming-call indication: the BIOS sets bit
			// 3 at status-block offset 4, which production PHONE.EXE explicitly
			// tests before entering its native answer/reject path.  A8h clears it.
			sio1_rx_byte(0xa7);
			// A real AMPS base station repeats the alert order until the call is
			// answered or released.  Repeating the edge prevents PHONE.EXE from
			// missing a one-shot indication while it has interrupts masked.
			rf_schedule_event(13, 20); // repeat after one second
			break;
		case 13:
			if (m_phone_ring && !m_answer_request_sent)
			{
				sio1_rx_byte(0x6a);
				rf_schedule_event(12, 4);
			}
			else
			{
				m_rf_event_phase = 0;
			}
			break;
		}
	}

	const u8 system = m_cellular_system->read() & 0x07;
	if (m_cellular_registration == 0xff && m_rf_control_mode && m_phone_power && system != m_last_cellular_system)
	{
		sio1_rx_byte(0x84 + system);
		m_last_cellular_system = system;
	}

	const bool incoming = BIT(m_cellular->read(), 0);
	if (incoming && !m_last_incoming)
	{
		// A key press latches the incoming call, matching a real ringing mobile;
		// releasing R must not make the network withdraw the page immediately.
		if (m_phone_ring)
		{
			m_phone_ring = false;
			sio1_rx_byte(0xa8);
			sio1_rx_byte(0x64);
		}
		else
		{
			cellular_start_ringing();
		}
	}
	m_last_incoming = incoming;
	uart_update_irq();
	sio1_update_irq();
}

void ibmsimon_state::io_map(address_map &map)
{
	map.unmap_value_high();
	map(0x0000, 0x000f).rw("mb:dma8237", FUNC(am9517a_device::read), FUNC(am9517a_device::write));
	map(0x0020, 0x002f).rw("mb:pic8259", FUNC(pic8259_device::read), FUNC(pic8259_device::write));
	map(0x0026, 0x0026).w(FUNC(ibmsimon_state::vg230_index_w));
	map(0x0027, 0x0027).rw(FUNC(ibmsimon_state::vg230_data_r), FUNC(ibmsimon_state::vg230_data_w));
	map(0x0040, 0x004f).rw("mb:pit8253", FUNC(pit8253_device::read), FUNC(pit8253_device::write));
	map(0x0060, 0x0060).lr8(NAME([this]() { return m_vg230_regs[0x0a]; }));
	map(0x0061, 0x0061).rw(FUNC(ibmsimon_state::ppi_b_r), FUNC(ibmsimon_state::ppi_b_w));
	map(0x0062, 0x0062).lrw8(NAME([this]() { return u8(0x01 | (m_mb->pit_out2() ? 0x20 : 0x00)); }), NAME([](u8) { }));
	map(0x006c, 0x006c).lrw8(NAME([this]() { return m_map_address; }), NAME([this](u8 data) { m_map_address = data; }));
	map(0x006e, 0x006f).rw(FUNC(ibmsimon_state::map_data_r), FUNC(ibmsimon_state::map_data_w));
	map(0x00a0, 0x00a0).w(m_mb, FUNC(pc_noppi_mb_device::nmi_enable_w));
	map(0x0080, 0x0083).lrw8(NAME([this](offs_t offset) { return m_dma_page_regs[offset]; }), NAME([this](offs_t offset, u8 data) { m_dma_page_regs[offset] = data; }));
	map(0x0170, 0x0170).rw(FUNC(ibmsimon_state::touch_data_r), FUNC(ibmsimon_state::touch_data_w));
	map(0x0172, 0x0172).rw(FUNC(ibmsimon_state::touch_control_r), FUNC(ibmsimon_state::touch_control_w));
	map(0x0278, 0x027a).rw(FUNC(ibmsimon_state::parallel_r), FUNC(ibmsimon_state::parallel_w));
	map(0x02f8, 0x02ff).rw(FUNC(ibmsimon_state::uart2_r), FUNC(ibmsimon_state::uart2_w));
	map(0x0378, 0x037a).rw(FUNC(ibmsimon_state::parallel_r), FUNC(ibmsimon_state::parallel_w));
	map(0x03f8, 0x03ff).rw(FUNC(ibmsimon_state::uart1_r), FUNC(ibmsimon_state::uart1_w));
	map(0x03b0, 0x03bf).rw(FUNC(ibmsimon_state::cga_r), FUNC(ibmsimon_state::cga_w));
	map(0x03bc, 0x03be).rw(FUNC(ibmsimon_state::parallel_r), FUNC(ibmsimon_state::parallel_w));
	map(0x03d0, 0x03df).rw(FUNC(ibmsimon_state::cga_r), FUNC(ibmsimon_state::cga_w));
}

u32 ibmsimon_state::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	// The green LED follows the VG230 system power state.  The amber LED is
	// driven by the RF deck supply and flashes while the deck is ringing.
	m_power_led = (m_vg230_regs[0xc0] & 0x03) != 0x03;
	m_phone_led = m_phone_power && (!m_phone_ring || BIT(machine().time().as_ticks(4), 0));
	m_backlight_led = m_lcd_backlight;

	// Simon's transflective LCD remains readable without its electroluminescent
	// backlight.  PWRON VP0 enables the panel supply while CFh's activity timer
	// controls the visible backlight state emulated above.
	const bool backlight = m_lcd_backlight;
	const rgb_t paper = backlight ? rgb_t(0xf4, 0xf2, 0xdf) : rgb_t(0xa9, 0xad, 0x91);
	const rgb_t ink = backlight ? rgb_t(0x18, 0x1b, 0x18) : rgb_t(0x43, 0x48, 0x3c);
	bitmap.fill(paper, cliprect);

	// VG230 mode 640x200 APA uses the PC-compatible CGA odd/even 8 KiB
	// scan-line banks.  Simon firmware selects this mode for its GUI.
	const unsigned start = (((unsigned(m_crtc_regs[0x0c]) & 0x3f) << 8) | m_crtc_regs[0x0d]) * 2;
	for (int y = cliprect.min_y; y <= cliprect.max_y && y < 200; ++y)
	{
		for (int x = cliprect.min_x; x <= cliprect.max_x && x < 640; ++x)
		{
			const unsigned address = start + ((y & 1) ? 0x2000 : 0) + (y >> 1) * 80 + (x >> 3);
			bitmap.pix(y, x) = BIT(m_video_ram[address & 0x7fff], 7 - (x & 7)) ? paper : ink;
		}
	}
	return 0;
}

static INPUT_PORTS_START(ibmsimon)
	PORT_START("PENX")
	// ROT90 displays native (x,y) at (199-y,x): host Y therefore drives
	// native X, while host X drives the reversed native Y axis.
	PORT_BIT(0x03ff, 0x013f, IPT_LIGHTGUN_Y) PORT_NAME("Pen Native X / Window Y") PORT_MINMAX(0, 639) PORT_SENSITIVITY(16) PORT_KEYDELTA(2) PORT_CROSSHAIR(X, 1.0, 0.0, 0)

	PORT_START("PENY")
	PORT_BIT(0x03ff, 0x0063, IPT_LIGHTGUN_X) PORT_NAME("Pen Native Y / Window X") PORT_MINMAX(0, 199) PORT_SENSITIVITY(50) PORT_KEYDELTA(3) PORT_REVERSE PORT_CROSSHAIR(Y, 1.0, 0.0, 0)

	PORT_START("PEN")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_BUTTON1) PORT_NAME("Touch Screen / Stylus") PORT_CODE(MOUSECODE_BUTTON1)

	PORT_START("CELLULAR")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_OTHER) PORT_NAME("Virtual 1G Incoming Call") PORT_CODE(KEYCODE_R)

	PORT_START("CELLULAR_SYSTEM")
	PORT_CONFNAME(0x07, 0x00, "Virtual 1G Registration")
	PORT_CONFSETTING(0x00, "Home 1")
	PORT_CONFSETTING(0x01, "Home 2")
	PORT_CONFSETTING(0x02, "Home 3")
	PORT_CONFSETTING(0x03, "Home 4")
	PORT_CONFSETTING(0x04, "Roam")
	PORT_CONFSETTING(0x05, "Alternate Roam")

INPUT_PORTS_END

void ibmsimon_state::ibmsimon(machine_config &config)
{
	V30(config, m_maincpu, 16_MHz_XTAL);
	m_maincpu->set_addrmap(AS_PROGRAM, &ibmsimon_state::mem_map);
	m_maincpu->set_addrmap(AS_IO, &ibmsimon_state::io_map);
	m_maincpu->set_irq_acknowledge_callback("mb:pic8259", FUNC(pic8259_device::inta_cb));

	PCNOPPI_MOTHERBOARD(config, m_mb);
	m_mb->set_cputag(m_maincpu);
	m_mb->int_callback().set_inputline(m_maincpu, 0);
	m_mb->nmi_callback().set_inputline(m_maincpu, INPUT_LINE_NMI);

	// The VG230 integrates an 8254, and Simon uses its read-back command to
	// time alert cadences.  An 8253 makes those waits complete immediately.
	pit8254_device &pit(PIT8254(config.replace(), "mb:pit8253"));
	pit.set_clk<0>(XTAL(14'318'181) / 12.0);
	pit.out_handler<0>().set("mb:pic8259", FUNC(pic8259_device::ir0_w));
	pit.set_clk<1>(XTAL(14'318'181) / 12.0);
	pit.out_handler<1>().set(m_mb, FUNC(pc_noppi_mb_device::pc_pit8253_out1_changed));
	// Simon also polls channel 2 as a software sound timebase.  Compensate its
	// effective speaker cadence independently; the unadjusted MAME PIT timing
	// makes both the touch click and incoming-call alert play at double speed.
	pit.set_clk<2>(XTAL(14'318'181) / 24.0);
	pit.out_handler<2>().set(m_mb, FUNC(pc_noppi_mb_device::pc_pit8253_out2_changed));

	// The VG230 data manual specifies a low-pass filter between SPKR and the
	// physical transducer.  Without it, the ideal one-bit square wave sounds
	// unnaturally harsh and its high harmonics resemble a sped-up chirp.
	filter_rc_device &speaker_filter(FILTER_RC(config, "speaker_filter"));
	speaker_filter.set_lowpass(RES_K(10), CAP_N(10)).add_route(ALL_OUTPUTS, "mb:mono", 1.0);
	subdevice<speaker_sound_device>("mb:speaker")->reset_routes().add_route(ALL_OUTPUTS, ":speaker_filter", 1.0);

	RAM(config, m_mainram).set_default_size("512K");

	screen_device &screen(SCREEN(config, "screen").set_lcd());
	screen.set_refresh_hz(70);
	screen.set_size(640, 200);
	screen.set_visarea_full();
	screen.set_orientation(ROT90);
	screen.set_screen_update(FUNC(ibmsimon_state::screen_update));

	// Host-side virtual AMPS/1G link.  Attach a file for protocol tracing or
	// socket.host:port to connect multiple Simon instances through a broker.
	BITBANGER(config, m_cellular_link).set_interface("simon_cellular");
	config.set_default_layout(layout_ibmsimon);
}

ROM_START(ibmsimon)
	ROM_REGION16_LE(0x20000, "bios", ROMREGION_ERASE00)
	ROM_LOAD("simonbios.bin", 0x00000, 0x20000, CRC(e47b0e90) SHA1(da6d878f130fcc012df5e1bbc9d42dda9ebce516))

	ROM_REGION(0x100000, "flash", 0)
	ROM_LOAD("simonflash.bin", 0x000000, 0x100000, CRC(00f5a976) SHA1(e8ac9d57613775400fbdb13f8e4343f0c3552139))
ROM_END

} // anonymous namespace

//    YEAR  NAME      PARENT  COMPAT  MACHINE   INPUT     CLASS           INIT        COMPANY  FULLNAME                              FLAGS
COMP(1994, ibmsimon, 0,      0,      ibmsimon, ibmsimon, ibmsimon_state, empty_init, "IBM",   "Simon Personal Communicator",       MACHINE_NOT_WORKING)
