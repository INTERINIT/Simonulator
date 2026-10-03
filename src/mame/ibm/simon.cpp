// license:BSD-3-Clause
// copyright-holders:OpenAI
/***************************************************************************

    IBM Simon Personal Communicator (1994)

    Early hardware bring-up driver.  Simon is built around the Vadem VG230,
    a 16 MHz NEC V30HL-based PC/XT-compatible system-on-chip.

    The current implementation models the PC/XT core and the VG230 indexed
    register and 16 KiB memory-window mechanisms.  LCD, pen digitizer and
    PCMCIA boot, touch, speaker and high-level cellular RF support are
    present.  Power management, AMPS call control, PCM voice and the onboard
    data/fax modem are modelled at the protocol level.

***************************************************************************/

#include "emu.h"

#include "cpu/nec/nec.h"
#include "bus/pccard/pccard.h"
#include "bus/pccard/ataflash.h"
#include "bus/pccard/linflash.h"
#include "bus/pccard/sram.h"
#include "machine/genpc.h"
#include "machine/keyboard.h"
#include "machine/ram.h"
#include "imagedev/bitbngr.h"
#include "imagedev/cassette.h"
#include "sound/dac.h"
#include "sound/flt_rc.h"

#include "crsshair.h"
#include "emupal.h"
#include "render.h"
#include "screen.h"

#include "ibmsimon.lh"

#include <cstdio>

namespace {

class ibmsimon_state : public driver_device
{
public:
	ibmsimon_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_maincpu(*this, "maincpu"),
		m_mb(*this, "mb"),
		m_screen(*this, "screen"),
		m_mainram(*this, RAM_TAG),
		m_bios(*this, "bios"),
		m_flash(*this, "flash"),
		m_pcmcia(*this, "pcmcia"),
		m_pen_x(*this, "PENX"),
		m_pen_y(*this, "PENY"),
		m_pen_landscape_x(*this, "PENLX"),
		m_pen_landscape_y(*this, "PENLY"),
		m_pen_button(*this, "PEN"),
		m_external_buttons(*this, "BUTTONS"),
		m_cellular_system(*this, "CELLULAR_SYSTEM"),
		m_cellular_link(*this, "cellular"),
		m_voice_input(*this, "voicein"),
		m_voice_dac(*this, "voice_dac"),
		m_power_led(*this, "power_led"),
		m_phone_led(*this, "phone_led"),
		m_backlight_led(*this, "backlight_led"),
		m_stylus_x(*this, "stylus_x"),
		m_stylus_y(*this, "stylus_y")
	{
	}

	void ibmsimon(machine_config &config);
	INPUT_CHANGED_MEMBER(power_button);
	INPUT_CHANGED_MEMBER(orientation_button);

protected:
	virtual void machine_start() override;
	virtual void machine_reset() override;

private:
	required_device<v30_device> m_maincpu;
	required_device<pc_noppi_mb_device> m_mb;
	required_device<screen_device> m_screen;
	required_device<ram_device> m_mainram;
	required_region_ptr<u16> m_bios;
	required_region_ptr<u8> m_flash;
	required_device<pccard_slot_device> m_pcmcia;
	required_ioport m_pen_x;
	required_ioport m_pen_y;
	required_ioport m_pen_landscape_x;
	required_ioport m_pen_landscape_y;
	required_ioport m_pen_button;
	required_ioport m_external_buttons;
	required_ioport m_cellular_system;
	required_device<bitbanger_device> m_cellular_link;
	required_device<cassette_image_device> m_voice_input;
	required_device<dac_byte_interface> m_voice_dac;
	output_finder<> m_power_led;
	output_finder<> m_phone_led;
	output_finder<> m_backlight_led;
	output_finder<> m_stylus_x;
	output_finder<> m_stylus_y;

	std::array<u8, 0x80000> m_upper_ram{};
	std::array<u8, 0x8000> m_video_ram{};
	std::array<u8, 0x100> m_vg230_regs{};
	emu_timer *m_rtc_timer = nullptr;
	s64 m_rtc_subsecond_remaining = ATTOSECONDS_PER_SECOND;
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
	bool m_pcmcia_cd1 = true;
	bool m_pcmcia_cd2 = true;
	bool m_pcmcia_cd1_target = true;
	bool m_pcmcia_cd2_target = true;
	bool m_pcmcia_contact_pending = false;
	bool m_pcmcia_bvd1 = true;
	bool m_pcmcia_bvd2 = true;
	bool m_pcmcia_wp = false;
	bool m_pcmcia_changed = false;
	bool m_pcmcia_change_ack_pending = false;
	bool m_pcmcia_timeout = false;
	bool m_pcmcia_ever_present = false;
	bool m_pcmcia_power_probe = false;
	std::array<u8, 0x2000> m_pcmcia_attribute{};
	u16 m_pcmcia_attribute_trace_count = 0;
	u16 m_pcmcia_attribute_write_trace_count = 0;
	u16 m_pcmcia_common_trace_count = 0;
	u16 m_pcmcia_volume_trace_count = 0;
	u16 m_pcmcia_map_trace_count = 0;
	u16 m_pcmcia_status_trace_count = 0;
	emu_timer *m_pcmcia_irq_retrigger_timer = nullptr;
	std::array<u8, 5> m_touch_packet{};
	u8 m_touch_packet_size = 0;
	u8 m_touch_packet_pos = 0;
	u8 m_board_data_latch = 0xff;
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
	bool m_sio1_rx_ready = false;
	bool m_sio1_tx_empty = true;
	bool m_sio1_thre_irq = false;
	std::array<u8, 256> m_modem_command{};
	u16 m_modem_command_size = 0;
	bool m_modem_echo = true;
	bool m_modem_verbose = true;
	bool m_modem_online = false;
	bool m_modem_dialing = false;
	bool m_modem_voice = false;
	u8 m_modem_escape_count = 0;
	u8 m_modem_fax_class = 0;
	std::array<s16, 205> m_modem_dtmf_window{};
	u16 m_modem_dtmf_count = 0;
	char m_modem_dtmf_candidate = 0;
	u8 m_modem_dtmf_stable = 0;
	char m_modem_dtmf_active = 0;
	std::array<u8, 9 * 33> m_pager_pages{};
	std::array<u8, 9> m_pager_lengths{};
	std::array<u8, 33> m_pager_capture{};
	u8 m_pager_capture_size = 0;
	bool m_pager_capture_started = false;
	bool m_pager_unread = false;
	u16 m_pager_beep_samples = 0;
	u16 m_pager_beep_phase = 0;
	bool m_rf_control_mode = false;
	bool m_rf_ready_announced = false;
	bool m_main_power = true;
	bool m_phone_power = false;
	bool m_phone_call = false;
	bool m_phone_ring = false;
	bool m_phone_muted = false;
	u8 m_phone_volume = 3;
	bool m_phone_accept_pages = false;
	bool m_phone_auto_answer = false;
	bool m_phone_auto_answering = false;
	u8 m_phone_ring_pulses = 0;
	bool m_network_busy_audio = false;
	u8 m_rf_call_state = 0x64;
	bool m_answer_request_sent = false;
	bool m_call_request_sent = false;
	u8 m_rf_tx_address = 0;
	bool m_rf_dial_frame = false;
	bool m_rf_dial_pending = false;
	bool m_rf_data_setup = false;
	bool m_rf_answer_prefix = false;
	bool m_rf_nam_number_pending = false;
	std::array<u8, 32> m_rf_dial_digits{};
	u8 m_rf_dial_size = 0;
	u8 m_rf_event_phase = 0;
	u8 m_rf_event_ticks = 0;
	bool m_lcd_backlight = true;
	bool m_landscape = false;
	u8 m_last_cellular_system = 0xff;
	u8 m_cellular_registration = 0xff;
	u8 m_cellular_signal = 6;
	u16 m_cellular_sid = 1;
	u16 m_cellular_channel = 334;
	std::array<u8, 10> m_cellular_number{};
	std::array<u8, 32> m_cellular_operator{};
	std::array<u8, 1024> m_cellular_link_line{};
	u16 m_cellular_link_line_size = 0;
	std::array<u8, 4096> m_voice_rx{};
	u16 m_voice_rx_head = 0;
	u16 m_voice_rx_tail = 0;
	u16 m_voice_rx_count = 0;
	bool m_voice_rx_started = false;
	std::array<u8, 160> m_voice_tx{};
	u8 m_voice_tx_count = 0;
	char m_dtmf_digit = 0;
	u16 m_dtmf_samples = 0;
	u32 m_dtmf_low_phase = 0;
	u32 m_dtmf_high_phase = 0;
	u16 m_cellular_sat = 6000;
	u8 m_sat_report_ticks = 0;
	emu_timer *m_cellular_timer = nullptr;
	emu_timer *m_cellular_link_timer = nullptr;
	emu_timer *m_voice_timer = nullptr;
	emu_timer *m_uart_tx_timer = nullptr;
	emu_timer *m_uart_rx_irq_timer = nullptr;
	emu_timer *m_sio1_rx_irq_timer = nullptr;
	emu_timer *m_sio1_tx_timer = nullptr;

	void mem_map(address_map &map) ATTR_COLD;
	void io_map(address_map &map) ATTR_COLD;
	u8 conventional_r(offs_t offset);
	void conventional_w(offs_t offset, u8 data);
	u8 window_r(offs_t offset);
	void window_w(offs_t offset, u8 data);
	u8 vg230_data_r();
	void vg230_data_w(u8 data);
	u8 rtc_data_r(u8 index) const;
	void rtc_data_w(u8 index, u8 data);
	void rtc_update_irq();
	TIMER_CALLBACK_MEMBER(rtc_tick);
	void update_cpu_clock();
	void pmu_resume(u8 source);
	void pmu_ring();
	void vg230_index_w(u8 data);
	void pcmcia_cd1_w(int state);
	void pcmcia_cd2_w(int state);
	void pcmcia_bvd1_w(int state);
	void pcmcia_bvd2_w(int state);
	void pcmcia_wp_w(int state);
	u8 pcmcia_status_r() const;
	void pcmcia_update_irq();
	void pcmcia_schedule_irq_retrigger();
	void pcmcia_attribute_init();
	u8 pcmcia_attribute_r(u32 address) const;
	void pcmcia_attribute_w(u32 address, u8 data);
	IRQ_CALLBACK_MEMBER(irq_acknowledge);
	u8 map_data_r(offs_t offset);
	void map_data_w(offs_t offset, u8 data);
	u8 cga_r(offs_t offset);
	void cga_w(offs_t offset, u8 data);
	u8 ppi_b_r();
	void ppi_b_w(u8 data);
	u8 ppi_c_r();
	void dos_keyboard_put(u8 data);
	u8 parallel_r(offs_t offset);
	void parallel_w(offs_t offset, u8 data);
	u8 touch_data_r();
	void touch_data_w(u8 data);
	u8 external_buttons_r();
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
	void rf_status_response(u8 query, u16 value);
	void rf_receive_byte(u8 data);
	void modem_receive_byte(u8 data);
	void modem_execute_command();
	void modem_voice_sample(u8 sample);
	void modem_response(std::string_view text);
	void modem_result(u8 numeric, std::string_view verbose);
	void cellular_link_output(u8 data);
	void cellular_link_text(std::string_view text);
	void rf_handset_command(u8 command);
	void rf_schedule_event(u8 phase, u8 ticks);
	void cellular_link_command();
	void cellular_report_network();
	void cellular_start_ringing(bool pager);
	TIMER_CALLBACK_MEMBER(touch_tick);
	TIMER_CALLBACK_MEMBER(pcmcia_irq_retrigger);
	TIMER_CALLBACK_MEMBER(cellular_tick);
	TIMER_CALLBACK_MEMBER(cellular_link_tick);
	TIMER_CALLBACK_MEMBER(voice_tick);
	TIMER_CALLBACK_MEMBER(uart_tx_done);
	TIMER_CALLBACK_MEMBER(uart_rx_irq);
	TIMER_CALLBACK_MEMBER(sio1_rx_irq);
	TIMER_CALLBACK_MEMBER(sio1_tx_done);
	u32 screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);
};

void ibmsimon_state::machine_start()
{
	save_item(NAME(m_upper_ram));
	save_item(NAME(m_video_ram));
	save_item(NAME(m_vg230_regs));
	save_item(NAME(m_rtc_subsecond_remaining));
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
	save_item(NAME(m_pcmcia_cd1));
	save_item(NAME(m_pcmcia_cd2));
	save_item(NAME(m_pcmcia_cd1_target));
	save_item(NAME(m_pcmcia_cd2_target));
	save_item(NAME(m_pcmcia_contact_pending));
	save_item(NAME(m_pcmcia_bvd1));
	save_item(NAME(m_pcmcia_bvd2));
	save_item(NAME(m_pcmcia_wp));
	save_item(NAME(m_pcmcia_changed));
	save_item(NAME(m_pcmcia_change_ack_pending));
	save_item(NAME(m_pcmcia_timeout));
	save_item(NAME(m_pcmcia_ever_present));
	save_item(NAME(m_pcmcia_power_probe));
	save_item(NAME(m_pcmcia_attribute));
	save_item(NAME(m_pcmcia_attribute_trace_count));
	save_item(NAME(m_pcmcia_attribute_write_trace_count));
	save_item(NAME(m_pcmcia_common_trace_count));
	save_item(NAME(m_pcmcia_volume_trace_count));
	save_item(NAME(m_pcmcia_map_trace_count));
	save_item(NAME(m_pcmcia_status_trace_count));
	save_item(NAME(m_touch_packet));
	save_item(NAME(m_touch_packet_size));
	save_item(NAME(m_touch_packet_pos));
	save_item(NAME(m_board_data_latch));
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
	save_item(NAME(m_sio1_rx_ready));
	save_item(NAME(m_sio1_tx_empty));
	save_item(NAME(m_sio1_thre_irq));
	save_item(NAME(m_modem_command));
	save_item(NAME(m_modem_command_size));
	save_item(NAME(m_modem_echo));
	save_item(NAME(m_modem_verbose));
	save_item(NAME(m_modem_online));
	save_item(NAME(m_modem_dialing));
	save_item(NAME(m_modem_voice));
	save_item(NAME(m_modem_escape_count));
	save_item(NAME(m_modem_fax_class));
	save_item(NAME(m_modem_dtmf_window));
	save_item(NAME(m_modem_dtmf_count));
	save_item(NAME(m_modem_dtmf_candidate));
	save_item(NAME(m_modem_dtmf_stable));
	save_item(NAME(m_modem_dtmf_active));
	save_item(NAME(m_pager_pages));
	save_item(NAME(m_pager_lengths));
	save_item(NAME(m_pager_capture));
	save_item(NAME(m_pager_capture_size));
	save_item(NAME(m_pager_capture_started));
	save_item(NAME(m_pager_unread));
	save_item(NAME(m_pager_beep_samples));
	save_item(NAME(m_pager_beep_phase));
	save_item(NAME(m_rf_control_mode));
	save_item(NAME(m_rf_ready_announced));
	save_item(NAME(m_main_power));
	save_item(NAME(m_phone_power));
	save_item(NAME(m_phone_call));
	save_item(NAME(m_phone_ring));
	save_item(NAME(m_phone_muted));
	save_item(NAME(m_phone_volume));
	save_item(NAME(m_phone_accept_pages));
	save_item(NAME(m_phone_auto_answer));
	save_item(NAME(m_phone_auto_answering));
	save_item(NAME(m_phone_ring_pulses));
	save_item(NAME(m_network_busy_audio));
	save_item(NAME(m_rf_call_state));
	save_item(NAME(m_answer_request_sent));
	save_item(NAME(m_call_request_sent));
	save_item(NAME(m_rf_tx_address));
	save_item(NAME(m_rf_dial_frame));
	save_item(NAME(m_rf_dial_pending));
	save_item(NAME(m_rf_data_setup));
	save_item(NAME(m_rf_answer_prefix));
	save_item(NAME(m_rf_nam_number_pending));
	save_item(NAME(m_rf_dial_digits));
	save_item(NAME(m_rf_dial_size));
	save_item(NAME(m_rf_event_phase));
	save_item(NAME(m_rf_event_ticks));
	save_item(NAME(m_lcd_backlight));
	save_item(NAME(m_landscape));
	save_item(NAME(m_last_cellular_system));
	save_item(NAME(m_cellular_registration));
	save_item(NAME(m_cellular_signal));
	save_item(NAME(m_cellular_sid));
	save_item(NAME(m_cellular_channel));
	save_item(NAME(m_cellular_number));
	save_item(NAME(m_cellular_operator));
	save_item(NAME(m_cellular_link_line));
	save_item(NAME(m_cellular_link_line_size));
	save_item(NAME(m_voice_rx));
	save_item(NAME(m_voice_rx_head));
	save_item(NAME(m_voice_rx_tail));
	save_item(NAME(m_voice_rx_count));
	save_item(NAME(m_voice_rx_started));
	save_item(NAME(m_voice_tx));
	save_item(NAME(m_voice_tx_count));
	save_item(NAME(m_dtmf_digit));
	save_item(NAME(m_dtmf_samples));
	save_item(NAME(m_dtmf_low_phase));
	save_item(NAME(m_dtmf_high_phase));
	save_item(NAME(m_cellular_sat));
	save_item(NAME(m_sat_report_ticks));

	// The VG230 RTC has its own 32.768 kHz supply/oscillator domain.  Its
	// one-second carry must continue while the V30 and PIT are suspended.
	m_rtc_timer = timer_alloc(FUNC(ibmsimon_state::rtc_tick), this);
	m_rtc_timer->adjust(attotime::from_seconds(1), 0, attotime::from_seconds(1));
	machine().save().register_postload(save_prepost_delegate(FUNC(ibmsimon_state::rtc_update_irq), this));
	m_touch_timer = timer_alloc(FUNC(ibmsimon_state::touch_tick), this);
	m_touch_timer->adjust(attotime::from_hz(1000), 0, attotime::from_hz(1000));
	m_pcmcia_irq_retrigger_timer = timer_alloc(FUNC(ibmsimon_state::pcmcia_irq_retrigger), this);
	m_cellular_timer = timer_alloc(FUNC(ibmsimon_state::cellular_tick), this);
	m_cellular_timer->adjust(attotime::from_hz(20), 0, attotime::from_hz(20));
	m_cellular_link_timer = timer_alloc(FUNC(ibmsimon_state::cellular_link_tick), this);
	m_cellular_link_timer->adjust(attotime::from_hz(100), 0, attotime::from_hz(100));
	m_voice_timer = timer_alloc(FUNC(ibmsimon_state::voice_tick), this);
	m_voice_timer->adjust(attotime::from_hz(8000), 0, attotime::from_hz(8000));
	m_uart_tx_timer = timer_alloc(FUNC(ibmsimon_state::uart_tx_done), this);
	m_uart_rx_irq_timer = timer_alloc(FUNC(ibmsimon_state::uart_rx_irq), this);
	m_sio1_rx_irq_timer = timer_alloc(FUNC(ibmsimon_state::sio1_rx_irq), this);
	m_sio1_tx_timer = timer_alloc(FUNC(ibmsimon_state::sio1_tx_done), this);

	// MAME hides a captured host cursor; keep its LCD-space pen pointer visible.
	auto &pointer = machine().crosshair().get_crosshair(0);
	pointer.set_mode(CROSSHAIR_VISIBILITY_ON);
	pointer.set_visible(true);
}

void ibmsimon_state::machine_reset()
{
	// Reset the CPU/controller, not the battery-backed RTC/CMOS domain.
	// Cold construction already initializes 70h-BFh to zero with VALID clear.
	std::fill(m_vg230_regs.begin(), m_vg230_regs.begin() + 0x70, 0);
	std::fill(m_vg230_regs.begin() + 0xc0, m_vg230_regs.end(), 0);
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
	m_vg230_regs[0x20] = 0x70; // Slot 0 enabled, Slot 1 disabled, status IRQ on IRQ7
	m_vg230_regs[0x2d] = 0xa0; // documented PC Card power-control reset value
	// Simon software configures the physical Type II socket as VG230 Slot 0.
	// Keep the legacy ready/present value at reset: Card Services depends on it
	// during boot even when no external card image is attached.
	m_vg230_regs[0x22] = 0xec;
	m_vg230_regs[0x28] = 0xfc;
	m_vg230_regs[0x38] = 0x28; // default top of conventional memory: 640 KiB
	m_vg230_regs[0x40] = 0xf0; // ICU shadow reserved bits
	m_vg230_regs[0xc1] = 0x01; // PMU registers start write-protected
	m_vg230_regs[0xc6] = 0xfe; // documented PMU PWRON reset value
	m_vg230_regs[0xcf] = 0x0a; // documented LCD inactivity default: 10 minutes
	update_cpu_clock();

	m_vg230_index = 0;
	m_map_address = 0;
	m_crtc_index = 0;
	m_cga_mode = 0;
	m_cga_mode_b = 0;
	m_cga_color = 0;
	m_ppi_b = 0;
	m_pcmcia_changed = false;
	m_pcmcia_change_ack_pending = false;
	m_pcmcia_timeout = false;
	m_pcmcia_cd1_target = m_pcmcia_cd1;
	m_pcmcia_cd2_target = m_pcmcia_cd2;
	m_pcmcia_contact_pending = false;
	m_pcmcia_ever_present = !m_pcmcia_cd1 && !m_pcmcia_cd2;
	m_pcmcia_power_probe = false;
	pcmcia_attribute_init();
	m_pcmcia_attribute_trace_count = 0;
	m_pcmcia_attribute_write_trace_count = 0;
	m_pcmcia_common_trace_count = 0;
	m_pcmcia_volume_trace_count = 0;
	m_pcmcia_map_trace_count = 0;
	m_pcmcia_status_trace_count = 0;
	m_pcmcia_irq_retrigger_timer->adjust(attotime::never);
	m_mb->m_pit8253->write_gate2(0);
	m_mb->pc_speaker_set_spkrdata(0);
	m_touch_packet.fill(0);
	m_touch_packet_size = 0;
	m_touch_packet_pos = 0;
	// Simon's external board-data latch powers up high.  Bit 7 is the
	// active-low green system LED, so the lamp remains dark until firmware
	// reaches the normal power-on sequence and explicitly asserts it.
	m_board_data_latch = 0xff;
	m_touch_control = 0;
	m_last_pen_x = 0xffff;
	m_last_pen_y = 0xffff;
	m_last_pen_down = false;
	m_touch_irq_pending = false;
	pcmcia_update_irq();
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
	m_sio1_rx_ready = false;
	m_sio1_tx_empty = true;
	m_sio1_thre_irq = false;
	m_sio1_rx_irq_timer->adjust(attotime::never);
	m_sio1_tx_timer->adjust(attotime::never);
	m_modem_command.fill(0);
	m_modem_command_size = 0;
	m_modem_echo = true;
	m_modem_verbose = true;
	m_modem_online = false;
	m_modem_dialing = false;
	m_modem_voice = false;
	m_modem_escape_count = 0;
	m_modem_fax_class = 0;
	m_modem_dtmf_window.fill(0);
	m_modem_dtmf_count = 0;
	m_modem_dtmf_candidate = 0;
	m_modem_dtmf_stable = 0;
	m_modem_dtmf_active = 0;
	m_pager_pages.fill(0);
	m_pager_lengths.fill(0);
	m_pager_capture.fill(0);
	m_pager_capture_size = 0;
	m_pager_capture_started = false;
	m_pager_unread = false;
	m_pager_beep_samples = 0;
	m_pager_beep_phase = 0;
	m_rf_control_mode = false;
	m_rf_ready_announced = false;
	m_main_power = true;
	m_phone_power = false;
	m_phone_call = false;
	m_phone_ring = false;
	m_phone_muted = false;
	m_phone_volume = 3;
	m_phone_accept_pages = false;
	m_phone_auto_answer = false;
	m_phone_auto_answering = false;
	m_phone_ring_pulses = 0;
	m_network_busy_audio = false;
	m_rf_call_state = 0x64;
	m_answer_request_sent = false;
	m_call_request_sent = false;
	m_rf_tx_address = 0;
	m_rf_dial_frame = false;
	m_rf_dial_pending = false;
	m_rf_data_setup = false;
	m_rf_answer_prefix = false;
	m_rf_nam_number_pending = false;
	m_rf_dial_digits.fill(0);
	m_rf_dial_size = 0;
	m_rf_event_phase = 0;
	m_rf_event_ticks = 0;
	m_lcd_backlight = false;
	m_landscape = false;
	// Do not inherit a landscape view remembered by MAME from a previous run.
	// A cold/reset Simon always starts in its normal portrait orientation.
	if (render_target *const target = machine().render().first_target())
		target->set_view(0);
	m_last_cellular_system = 0xff;
	m_cellular_registration = 0xff;
	m_cellular_signal = 6;
	m_cellular_sid = 1;
	m_cellular_channel = 334;
	m_cellular_number.fill('0');
	m_cellular_operator.fill(0);
	m_cellular_link_line_size = 0;
	m_voice_rx.fill(0x80);
	m_voice_rx_head = 0;
	m_voice_rx_tail = 0;
	m_voice_rx_count = 0;
	m_voice_rx_started = false;
	m_voice_tx.fill(0x80);
	m_voice_tx_count = 0;
	m_dtmf_digit = 0;
	m_dtmf_samples = 0;
	m_dtmf_low_phase = 0;
	m_dtmf_high_phase = 0;
	m_cellular_sat = 6000;
	m_sat_report_ticks = 0;
	m_voice_dac->data_w(0x80);
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
	// VG230 fetches its PMU NMI vector from ROM even when RAM is mapped here.
	// Reading C4h releases the hardware vector overlay.
	if (BIT(m_vg230_regs[0x19], 0) && offset >= 8 && offset < 12)
		return reinterpret_cast<const u8 *>(&m_bios[0])[offset];
	return m_mainram->pointer()[offset];
}

void ibmsimon_state::conventional_w(offs_t offset, u8 data)
{
	m_mainram->pointer()[offset] = data;
}

u8 ibmsimon_state::window_r(offs_t offset)
{
	const u32 address = 0x80000 + offset;

	// VG230 reserves only the 32 KiB B8000-BFFFF LCD display window.  B0000
	// and B4000 remain mapper pages and are used by Card Services as its PC
	// Card memory aperture.
	if (address >= 0xb8000 && address <= 0xbffff)
		return m_video_ram[address - 0xb8000];

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
		case 4: // PC Card A / Simon external Type II slot
			if (!m_pcmcia_cd1 && !m_pcmcia_cd2 && !BIT(m_vg230_regs[0x20], 7) && !BIT(pcmcia_status_r(), 0))
			{
				// VG230 slot-control D5 normally drives *REG: zero selects
				// attribute memory and one selects common memory.  Simon's Award
				// CardWare leaves D5 low, however, and its VG230 Socket Services
				// encodes a common-memory mapping by setting card-address A24.
				// The primary attribute CIS is consequently mapped below 16 MiB;
				// following a LONGLINK_C remaps the same card offset with A24 set.
				// A24 is a software tag here and must not reach a 1 MiB card's
				// backing store.  During DMA the VG230 also forces common memory.
				const bool enhanced_attribute_windows = BIT(m_vg230_regs[0xe0], 7) && BIT(m_vg230_regs[0xe0], 2);
				const bool common = m_mb->dma_memory_cycle() || enhanced_attribute_windows || BIT(m_vg230_regs[0x21], 5) || BIT(physical, 24);
				const u32 card_address = common ? (physical & 0x00ffffff) : physical;
				const u8 data = common ? m_pcmcia->read_memory_byte(card_address) : pcmcia_attribute_r(card_address);
				if (common && card_address >= 0x001f00 && card_address < 0x002400 && m_pcmcia_volume_trace_count < 512)
				{
					logerror("%s Simon PC Card volume read host=%05X card=%06X data=%02X\n", machine().describe_context(), address, card_address, data);
					++m_pcmcia_volume_trace_count;
				}
				u16 &trace_count = common
					? m_pcmcia_common_trace_count
					: m_pcmcia_attribute_trace_count;
				if (trace_count < 2048)
				{
					if (common)
						logerror("%s Simon PC Card common read host=%05X card=%06X data=%02X CTL=%02X\n", machine().describe_context(), address, card_address, data, m_vg230_regs[0x21]);
					else
						logerror("%s Simon PC Card attribute read %06X = %02X (CTL=%02X)\n", machine().describe_context(), card_address, data, m_vg230_regs[0x21]);
					++trace_count;
				}
				return data;
			}
			return 0xff;
		case 6: // PC Card A attribute memory in VG230 V3.1 window mode
			if (!m_pcmcia_cd1 && !m_pcmcia_cd2 && BIT(m_vg230_regs[0xe0], 7) && BIT(m_vg230_regs[0xe0], 2) && !BIT(m_vg230_regs[0x20], 7) && !BIT(pcmcia_status_r(), 0))
			{
				const u8 data = pcmcia_attribute_r(physical);
				if (m_pcmcia_attribute_trace_count < 2048)
					logerror("%s Simon PC Card attribute read %06X = %02X (DTYP=6)\n", machine().describe_context(), physical, data);
				++m_pcmcia_attribute_trace_count;
				return data;
			}
			return 0xff;
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
	if (address >= 0xb8000 && address <= 0xbffff)
	{
		m_video_ram[address - 0xb8000] = data;
		return;
	}

	const unsigned page = address >> 14;
	const u8 high = m_map_high[page];
	if (BIT(m_vg230_regs[0x04], 7) && BIT(high, 7))
	{
		const u32 physical = (u32(high & 0x0f) << 22) | (u32(m_map_low[page]) << 14) | (address & 0x3fff);
		switch ((high >> 4) & 0x07)
		{
		case 1: // system RAM
			if (physical < m_mainram->size())
				m_mainram->pointer()[physical] = data;
			else
				m_upper_ram[(physical - m_mainram->size()) & (m_upper_ram.size() - 1)] = data;
			break;
		case 4: // PC Card A / Simon external Type II slot
			if (!m_pcmcia_cd1 && !m_pcmcia_cd2 && !BIT(m_vg230_regs[0x20], 7) && !BIT(pcmcia_status_r(), 0))
			{
				const bool enhanced_attribute_windows = BIT(m_vg230_regs[0xe0], 7) && BIT(m_vg230_regs[0xe0], 2);
				const bool common = m_mb->dma_memory_cycle() || enhanced_attribute_windows || BIT(m_vg230_regs[0x21], 5) || BIT(physical, 24);
				const u32 card_address = common ? (physical & 0x00ffffff) : physical;
				if (common)
				{
					if (card_address >= 0x001f00 && card_address < 0x002400 && m_pcmcia_volume_trace_count < 512)
					{
						logerror("%s Simon PC Card volume write host=%05X card=%06X data=%02X\n", machine().describe_context(), address, card_address, data);
						++m_pcmcia_volume_trace_count;
					}
					if (m_pcmcia_common_trace_count < 256)
						osd_printf_info("Simon PC Card common write host=%05X card=%06X data=%02X CTL=%02X\n", address, card_address, data, m_vg230_regs[0x21]);
					++m_pcmcia_common_trace_count;
					m_pcmcia->write_memory_byte(card_address, data);
				}
				else
				{
					if (m_pcmcia_attribute_write_trace_count < 256)
						logerror("%s Simon PC Card attribute write %06X = %02X (CTL=%02X)\n", machine().describe_context(), card_address, data, m_vg230_regs[0x21]);
					++m_pcmcia_attribute_write_trace_count;
					pcmcia_attribute_w(card_address, data);
				}
			}
			break;
		case 6: // PC Card A attribute memory in VG230 V3.1 window mode
			if (!m_pcmcia_cd1 && !m_pcmcia_cd2 && BIT(m_vg230_regs[0xe0], 7) && BIT(m_vg230_regs[0xe0], 2) && !BIT(m_vg230_regs[0x20], 7) && !BIT(pcmcia_status_r(), 0))
			{
				if (m_pcmcia_attribute_write_trace_count < 256)
					logerror("%s Simon PC Card attribute write %06X = %02X (DTYP=6)\n", machine().describe_context(), physical, data);
				++m_pcmcia_attribute_write_trace_count;
				pcmcia_attribute_w(physical, data);
			}
			break;
		default:
			break;
		}
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

u8 ibmsimon_state::pcmcia_status_r() const
{
	const bool physical_present = !m_pcmcia_cd1 && !m_pcmcia_cd2;
	// Simon's original Socket Services hangs during cold boot if its only
	// socket reports absent before Card Services is resident.  Preserve the
	// established boot-time electrically-ready value until the first real card
	// insertion; after that, hot removal is reported with PRESENT deasserted.
	const bool present = physical_present || !m_pcmcia_ever_present;
	u8 result = 0;
	result |= 0x80; // SRAM cards are always ready
	result |= (!present || m_pcmcia_bvd2) ? 0x40 : 0x00;
	result |= (!present || m_pcmcia_bvd1) ? 0x20 : 0x00;
	result |= present ? 0x00 : 0x10;
	result |= m_pcmcia_changed ? 0x00 : 0x08;
	result |= m_pcmcia_timeout ? 0x00 : 0x04;
	result |= (present && m_pcmcia_wp) ? 0x02 : 0x00;
	// CRDOFF reports interface power, not card presence.  An empty socket does
	// not by itself turn this bit on (VG230 2Dh explicitly ignores VPCRD while
	// both sockets are empty).  Activity-timer power removal is not modelled.
	result |= 0x00;
	return result;
}

void ibmsimon_state::pcmcia_update_irq()
{
	// VG230 23h bit 3 is active as a mask: zero enables the latched card-change
	// interrupt.  Controller register 20h bits 5:4 route it to NMI/IRQ2/IRQ6/IRQ7.
	// Keep IRQ6 wired-OR with the digitizer, as it is on the real board.
	const bool pending = m_pcmcia_changed && !BIT(m_vg230_regs[0x23], 3);
	const u8 route = (m_vg230_regs[0x20] >> 4) & 0x03;
	m_maincpu->set_input_line(INPUT_LINE_NMI, (BIT(m_vg230_regs[0x19], 0) || (pending && route == 0)) ? ASSERT_LINE : CLEAR_LINE);
	rtc_update_irq(); // IRQ2 is shared with the independently running RTC.
	m_mb->m_pic8259->ir6_w(m_touch_irq_pending || (pending && route == 2));
	m_mb->m_pic8259->ir7_w(pending && route == 3);
}

void ibmsimon_state::pcmcia_schedule_irq_retrigger()
{
	// Debounce the two physical card-detect contacts into one stable socket
	// transition.  MAME invokes CD1 and CD2 callbacks consecutively for an image
	// operation; exposing the intermediate half-seated state and interrupting on
	// both callbacks can re-enter Simon's 1993 Card Services removal handler.
	// Slot line callbacks may run while devices are being started, before the
	// driver's timers have been allocated.  A boot-time card does not need a
	// hot-plug edge in any case.
	if (m_pcmcia_irq_retrigger_timer)
		m_pcmcia_irq_retrigger_timer->adjust(attotime::from_msec(20));
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::pcmcia_irq_retrigger)
{
	if (m_pcmcia_contact_pending)
	{
		const bool was_present = !m_pcmcia_cd1 && !m_pcmcia_cd2;
		const bool contacts_changed =
			(m_pcmcia_cd1 != m_pcmcia_cd1_target) ||
			(m_pcmcia_cd2 != m_pcmcia_cd2_target);
		m_pcmcia_cd1 = m_pcmcia_cd1_target;
		m_pcmcia_cd2 = m_pcmcia_cd2_target;
		m_pcmcia_contact_pending = false;
		const bool present = !m_pcmcia_cd1 && !m_pcmcia_cd2;
		// Notify Socket Services for either stable seating transition.  Simon's
		// resident Card Services uses the same status-change path to unmount an
		// ejected volume and to enumerate a newly inserted card.
		if (contacts_changed)
		{
			m_pcmcia_changed = true;
			m_pcmcia_change_ack_pending = false;
			// Supply the first stale identity sample only on removal.  A newly
			// seated card already identifies socket 0 through PRESENT.
			m_pcmcia_power_probe = !present;
		}
		if (present != was_present)
		{
			m_pcmcia_status_trace_count = 0;
			if (present)
			{
				m_pcmcia_ever_present = true;
				m_pcmcia_power_probe = false;
				// Arm status-change delivery before exposing the card's CIS.
				m_vg230_regs[0x23] &= ~u8(0x08);
				pcmcia_attribute_init();
			}
			logerror("Simon PC Card %s after CD2 settle (CD1=%d CD2=%d status=%02X ctl=%02X mask=%02X picmask=%02X)\n",
				present ? "inserted" : "removed", m_pcmcia_cd1, m_pcmcia_cd2,
				pcmcia_status_r(), m_vg230_regs[0x20], m_vg230_regs[0x23], m_mb->m_pic8259->read(1));
		}
	}
	pcmcia_update_irq();
}

void ibmsimon_state::pcmcia_attribute_init()
{
	m_pcmcia_attribute.fill(0xff);

	const bool sram_1800k = bool(m_pcmcia->subdevice("melcard_1800k"));
	const bool sram_4m = bool(m_pcmcia->subdevice("melcard_4m"));
	const bool sram_card = bool(m_pcmcia->subdevice("melcard_1m")) || sram_1800k || sram_4m;
	const bool linear_flash_card = bool(m_pcmcia->subdevice("linear16"));
	if (!sram_card && !linear_flash_card)
		return;

	// A memory-only PC Card stores the CIS in attribute space on even byte
	// addresses.  The backing Mitsubishi image contains common memory only, so
	// supply the CIS of a contemporary Centennial SL01M SRAM card.  In
	// particular, Simon's 1993 Award Card Services expects the PCMCIA 1.1
	// version tuple and the memory geometry tuple used by these cards.
	static constexpr u8 sram_cis[] = {
		// Smart/Centennial document 33311004 Rev C, "Card using
		// 4Mbit components", 1 MiB model.  Keep the tuple-internal FF
		// terminators: they are data covered by each tuple's link count.
		0x01, 0x03, 0x63, 0x0d, 0xff,                         // DEVICE: SRAM, 150 ns, 1 MiB
		0x1e, 0x07, 0x02, 0x00, 0x01, 0x01, 0x01, 0x01, 0xff, // DEVICE_GEO
		// Simon's 1993 Award CardWare overflows an internal product-info
		// buffer on the much longer VERS_1 strings found in the 2006 Smart
		// datasheet.  The strings are descriptive only; DEVICE and DEVICE_GEO
		// above carry the memory technology, speed, geometry and capacity.
		0x15, 0x1f, 0x04, 0x01,                               // VERS_1: PCMCIA 1.1 / JEIDA 4.2
		'C', 'e', 'n', 't', 'e', 'n', 'n', 'i', 'a', 'l', 0x00,
		'S', 'L', '0', '1', 'M', 0x00,
		'1', ' ', 'M', 'B', ' ', 'S', 'R', 'A', 'M', 0x00,
		0x00, 0xff,                                           // empty fourth string, end of VERS_1
		// Datalight CardTrick stores its disk-format tuples in a secondary
		// chain at common-memory byte address zero.  The LINKTARGET("CIS")
		// tuple at that address is only valid when the primary attribute CIS
		// links to it explicitly.
		0x12, 0x04, 0x00, 0x00, 0x00, 0x00,                   // LONGLINK_C -> common 0
		0xff                                                  // end of CIS chain
	};
	static constexpr u8 sram_4m_cis[] = {
		0x01, 0x03, 0x63, 0x0e, 0xff,                         // DEVICE: SRAM, 150 ns, 4 MiB
		0x1e, 0x07, 0x02, 0x00, 0x01, 0x01, 0x01, 0x01, 0xff, // DEVICE_GEO
		0x15, 0x1f, 0x04, 0x01,
		'C', 'e', 'n', 't', 'e', 'n', 'n', 'i', 'a', 'l', 0x00,
		'S', 'L', '0', '4', 'M', 0x00,
		'4', ' ', 'M', 'B', ' ', 'S', 'R', 'A', 'M', 0x00,
		0x00, 0xff,
		0x12, 0x04, 0x00, 0x00, 0x00, 0x00,
		0xff
	};
	static constexpr u8 sram_1800k_cis[] = {
		// DEVICE length encoding: fourteen 128 KiB units = 1,792 KiB,
		// the capacity sold as the Simon 1.8 MB memory card.
		0x01, 0x03, 0x63, 0x6c, 0xff,
		0x1e, 0x07, 0x02, 0x00, 0x01, 0x01, 0x01, 0x01, 0xff,
		0x15, 0x1f, 0x04, 0x01,
		'C', 'e', 'n', 't', 'e', 'n', 'n', 'i', 'a', 'l', 0x00,
		'S', 'L', '1', '8', 'M', 0x00,
		'1', '.', '8', 'M', ' ', 'S', 'R', 'A', 'M', 0x00,
		0x00, 0xff,
		0x12, 0x04, 0x00, 0x00, 0x00, 0x00,
		0xff
	};
	// Simon was sold with 1 MiB and 1.8 MiB linear Flash memory cards.  Use the
	// compact CIS layout of a contemporary Series-C 1 MiB linear Flash card.
	// Long product strings from newer cards overrun the fixed product buffer in
	// the 1993 Award CardWare bundled with Simon and corrupt its resident driver.
	static constexpr u8 linear_flash_cis[] = {
		0x01, 0x03, 0x53, 0x0d, 0xff,                         // DEVICE: 150 ns Flash, 1 MiB
		0x18, 0x03, 0x89, 0xa2, 0xff,                         // JEDEC: Intel 28F008SA
		0x15, 0x1c, 0x04, 0x01,                               // VERS_1: PCMCIA 2.0 / JEIDA 4.1
		'C', '-', 'O', 'N', 'E', 0x00,
		'S', 'E', 'R', 'I', 'E', 'S', '-', 'C', 0x00,
		'1', 'M', 'B', ' ', 'F', 'L', 'A', 'S', 'H', 0x00,
		0xff,                                                  // end of product strings
		0x12, 0x04, 0x00, 0x00, 0x00, 0x00,                   // LONGLINK_C -> common 0
		0xff                                                   // end of CIS chain
	};

	const u8 *const cis = linear_flash_card ? linear_flash_cis : (sram_4m ? sram_4m_cis : (sram_1800k ? sram_1800k_cis : sram_cis));
	const size_t cis_size = linear_flash_card ? std::size(linear_flash_cis) : (sram_4m ? std::size(sram_4m_cis) : (sram_1800k ? std::size(sram_1800k_cis) : std::size(sram_cis)));
	std::copy_n(cis, cis_size, m_pcmcia_attribute.begin());

	// An unformatted SRAM card has no secondary common-memory CIS yet.  Do not
	// advertise a dangling long link: Card Services treats an invalid linked
	// chain as a malformed card, preventing Simon's own Prepare command from
	// reaching the formatter.  CardTrick's formatted chain starts with the
	// interleaved LINKTARGET tuple 13 03 "CIS" at common byte address zero.
	const bool cardtrick_chain =
		(m_pcmcia->read_memory_byte(0) == 0x13) &&
		(m_pcmcia->read_memory_byte(2) == 0x03) &&
		(m_pcmcia->read_memory_byte(4) == 'C') &&
		(m_pcmcia->read_memory_byte(6) == 'I') &&
		(m_pcmcia->read_memory_byte(8) == 'S');
	if (!cardtrick_chain)
	{
		// Both HLE templates place LONGLINK_C immediately before END.
		m_pcmcia_attribute[cis_size - 7] = 0xff;
		std::fill(m_pcmcia_attribute.begin() + cis_size - 6, m_pcmcia_attribute.begin() + cis_size, 0xff);
	}
}

u8 ibmsimon_state::pcmcia_attribute_r(u32 address) const
{
	// I/O cards such as ATA Flash provide their own CIS and configuration
	// registers through the generic PC Card device.
	const bool hle_memory_card = bool(m_pcmcia->subdevice("melcard_1m")) || bool(m_pcmcia->subdevice("melcard_1800k")) || bool(m_pcmcia->subdevice("melcard_4m")) || bool(m_pcmcia->subdevice("linear16"));
	if (!hle_memory_card)
		return m_pcmcia->read_reg_byte(address);

	// The 2 KiB 28C16 attribute EEPROM is selected on even byte addresses.
	// Its A0-A10 inputs are driven by PC Card A1-A11, so the contents repeat
	// over each 4 KiB host-address interval while A14 is low.  A14 disables
	// the EEPROM, and the whole decode repeats every 32 KiB (Smart/Centennial
	// document 33311004 Rev C, page 9).
	address &= 0x7fff;
	if (BIT(address, 14) || BIT(address, 0))
		return 0xff;
	return m_pcmcia_attribute[(address & 0x0fff) >> 1];
}

void ibmsimon_state::pcmcia_attribute_w(u32 address, u8 data)
{
	const bool hle_memory_card = bool(m_pcmcia->subdevice("melcard_1m")) || bool(m_pcmcia->subdevice("melcard_1800k")) || bool(m_pcmcia->subdevice("melcard_4m")) || bool(m_pcmcia->subdevice("linear16"));
	if (!hle_memory_card)
	{
		m_pcmcia->write_reg_byte(address, data);
		return;
	}

	// Keep the factory CIS protected.  CardWare issues generic JEDEC probe
	// commands while identifying memory technology; treating those commands as
	// ordinary EEPROM byte writes destroys tuple zero before enumeration ends.
}

IRQ_CALLBACK_MEMBER(ibmsimon_state::irq_acknowledge)
{
	const int vector = m_mb->m_pic8259->acknowledge();
	if (m_pcmcia_changed)
		logerror("Simon interrupt acknowledge vector=%02X while PC Card change is pending\n", vector);
	return vector;
}

void ibmsimon_state::pcmcia_cd1_w(int state)
{
	if (m_pcmcia_irq_retrigger_timer)
	{
		m_pcmcia_cd1_target = bool(state);
		m_pcmcia_contact_pending = true;
		pcmcia_schedule_irq_retrigger();
		return;
	}

	// Device-start callback, before the debounce timer exists.
	m_pcmcia_cd1 = m_pcmcia_cd1_target = bool(state);
}

void ibmsimon_state::pcmcia_cd2_w(int state)
{
	if (m_pcmcia_irq_retrigger_timer)
	{
		m_pcmcia_cd2_target = bool(state);
		m_pcmcia_contact_pending = true;
		pcmcia_schedule_irq_retrigger();
		return;
	}

	// Device-start callback, before the debounce timer exists.
	m_pcmcia_cd2 = m_pcmcia_cd2_target = bool(state);
}

void ibmsimon_state::pcmcia_bvd1_w(int state)
{
	m_pcmcia_bvd1 = bool(state);
}

void ibmsimon_state::pcmcia_bvd2_w(int state)
{
	m_pcmcia_bvd2 = bool(state);
}

void ibmsimon_state::pcmcia_wp_w(int state)
{
	m_pcmcia_wp = bool(state);
}

u8 ibmsimon_state::rtc_data_r(u8 index) const
{
	// *RTCEN/*RAMEN inhibit access, not the battery-powered clock itself.
	// VG230 Data Manual, RTC register description (indices 70h-BFh).
	if ((index <= 0x78 && BIT(m_vg230_regs[0x79], 7)) ||
		(index >= 0x80 && BIT(m_vg230_regs[0x79], 6)))
		return 0xff;
	if (index >= 0x7b && index <= 0x7f)
		return 0;
	return m_vg230_regs[index];
}

void ibmsimon_state::rtc_data_w(u8 index, u8 data)
{
	if ((index <= 0x78 && BIT(m_vg230_regs[0x79], 7)) ||
		(index >= 0x80 && BIT(m_vg230_regs[0x79], 6)))
		return;

	if (index <= 0x78)
	{
		// All fields are binary, not the BCD registers of a PC/AT MC146818.
		static constexpr u8 masks[] = { 0x3f, 0x3f, 0x1f, 0xff, 0x0f, 0x3f, 0x3f, 0x1f, 0x1f };
		m_vg230_regs[index] = data & masks[index - 0x70];
	}
	else if (index == 0x79)
	{
		const bool paused = BIT(m_vg230_regs[0x79], 5);
		m_vg230_regs[index] = data & 0xe3;
		if (paused != BIT(data, 5))
		{
			if (BIT(data, 5))
			{
				m_rtc_subsecond_remaining = m_rtc_timer->remaining().as_attoseconds();
				m_rtc_timer->adjust(attotime::never);
			}
			else
			{
				const attotime remaining(m_rtc_subsecond_remaining / ATTOSECONDS_PER_SECOND,
					m_rtc_subsecond_remaining % ATTOSECONDS_PER_SECOND);
				m_rtc_timer->adjust(remaining, 0, attotime::from_seconds(1));
			}
		}
		rtc_update_irq();
	}
	else if (index == 0x7a)
	{
		// VALID is set by software and lost only with RTC power.  In particular,
		// BIOS resume writes 02h to acknowledge ALARM; this must not erase VALID.
		// ALARM/PERIODIC are separate write-one-to-clear interrupt latches.
		m_vg230_regs[index] = (m_vg230_regs[index] | (data & 0x80)) & ~(data & 0x03);
		rtc_update_irq();
	}
	else if (index >= 0x80)
	{
		m_vg230_regs[index] = data;
	}
}

void ibmsimon_state::rtc_update_irq()
{
	const bool rtc_pending = (m_vg230_regs[0x7a] & m_vg230_regs[0x79] & 0x03) != 0;
	const bool card_pending = m_pcmcia_changed && !BIT(m_vg230_regs[0x23], 3) &&
		((m_vg230_regs[0x20] >> 4) & 0x03) == 1;
	m_mb->m_pic8259->ir2_w(rtc_pending || card_pending);
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::rtc_tick)
{
	if (BIT(m_vg230_regs[0x79], 5))
		return;

	if (++m_vg230_regs[0x70] >= 60)
	{
		m_vg230_regs[0x70] = 0;
		if (++m_vg230_regs[0x71] >= 60)
		{
			m_vg230_regs[0x71] = 0;
			if (++m_vg230_regs[0x72] >= 24)
			{
				m_vg230_regs[0x72] = 0;
				if (++m_vg230_regs[0x73] == 0)
					m_vg230_regs[0x74] = (m_vg230_regs[0x74] + 1) & 0x0f;
			}
		}
	}

	if (BIT(m_vg230_regs[0x79], 0))
		m_vg230_regs[0x7a] |= 0x01;
	const bool alarm = BIT(m_vg230_regs[0x79], 1) &&
		m_vg230_regs[0x70] == m_vg230_regs[0x75] &&
		m_vg230_regs[0x71] == m_vg230_regs[0x76] &&
		m_vg230_regs[0x72] == m_vg230_regs[0x77] &&
		(m_vg230_regs[0x73] & 0x1f) == m_vg230_regs[0x78];
	if (alarm)
		m_vg230_regs[0x7a] |= 0x02;
	rtc_update_irq();
	if (alarm)
		pmu_resume(2); // An RTC alarm can wake SUSPEND without a CPU interrupt.
}

u8 ibmsimon_state::vg230_data_r()
{
	if (m_vg230_index >= 0x70 && m_vg230_index <= 0xbf)
		return rtc_data_r(m_vg230_index);
	u8 result;
	if (m_vg230_index == 0xcb)
	{
		// OUTPUT reflects the current state's PWR register.  BIOS polls VP0
		// during both sides of its LCD power sequencing.
		result = m_vg230_regs[0xc6 + (m_vg230_regs[0xc0] & 3)];
	}
	else if (m_vg230_index == 0x30 || m_vg230_index == 0x31)
	{
		const u16 timebase = u16(machine().time().as_ticks(1'193'182));
		result = (m_vg230_index == 0x30) ? u8(timebase) : u8(timebase >> 8);
	}
	else
	{
		if (m_vg230_index == 0x22)
			result = pcmcia_status_r();
		else if (m_vg230_index == 0x28)
			result = 0xfc;
		else
			result = m_vg230_regs[m_vg230_index];
		// F000:CB25 is the generic two-socket "select operation socket" helper.
		// Simon only wires socket 0, and the helper has no both-empty exit: it
		// alternates reads at CB37/CB41 until one socket identifies itself.  A
		// removal operation can call it more than once, so preserving just one
		// stale PRESENT sample is insufficient.  Identify the former socket only
		// for the helper's socket-0 read; all of its callers' later status reads
		// still see the real empty state (PRESENT=1), including the removal tests
		// at C311, C333 and C37A.
		const bool removed_socket_select =
			(m_vg230_index == 0x22) &&
			(m_maincpu->pc() == 0xfcb37) &&
			m_pcmcia_ever_present && m_pcmcia_cd1 && m_pcmcia_cd2;
		if (removed_socket_select || (m_vg230_index == 0x22 && m_pcmcia_power_probe))
		{
			result &= ~u8(0x10);
			m_pcmcia_power_probe = false;
		}
		if ((m_vg230_index == 0x22 || m_vg230_index == 0x28) && m_pcmcia_status_trace_count < 256)
		{
			logerror("VG230 PC Card status pc=%05X slot=%u value=%02X external_present=%u CD1=%u CD2=%u\n",
				m_maincpu->pc(),
				(m_vg230_index == 0x28) ? 1 : 0, result, (!m_pcmcia_cd1 && !m_pcmcia_cd2) ? 1 : 0,
				m_pcmcia_cd1 ? 1 : 0, m_pcmcia_cd2 ? 1 : 0);
			++m_pcmcia_status_trace_count;
		}
	}
	// A read unlocks PMU writes after reset/resume.  Keep the exposed lock bit
	// faithful while allowing the following firmware write to proceed.
	if (m_vg230_index == 0xc1 && !machine().side_effects_disabled())
		m_vg230_regs[0xc1] &= ~u8(0x01);
	if (m_vg230_index == 0xc0 && !machine().side_effects_disabled())
		m_vg230_regs[0xc0] &= ~u8(0x80); // RESUME is read-to-clear
	if (m_vg230_index == 0xc4 && !machine().side_effects_disabled())
	{
		m_vg230_regs[0x19] &= ~u8(0x01);
		m_vg230_regs[0xc0] &= ~u8(0x1c);
		pcmcia_update_irq();
	}

	return result;
}

void ibmsimon_state::vg230_data_w(u8 data)
{
	if (m_vg230_index >= 0x70 && m_vg230_index <= 0xbf)
	{
		rtc_data_w(m_vg230_index, data);
		return;
	}
	if (m_vg230_index >= 0xc0 && m_vg230_index <= 0xdb && BIT(m_vg230_regs[0xc1], 0))
		return;
	// Card status pins are read-only, while writing one to CRDCHG/CRDTMO
	// acknowledges those two latched conditions.
	if (m_vg230_index == 0x22)
	{
		if ((BIT(data, 3) && m_pcmcia_changed) || (BIT(data, 2) && m_pcmcia_timeout))
			logerror("Simon PC Card status acknowledged value=%02X status-before=%02X\n", data, pcmcia_status_r());
		if (BIT(data, 3))
		{
			if (m_pcmcia_changed && !m_pcmcia_change_ack_pending)
			{
				// Simon's Socket Services deliberately acknowledges once before
				// sampling CRDCHG at C333, then acknowledges again at C39A.  The
				// original controller keeps the removal indication visible across
				// that first write.  Complete the clear on the second write so the
				// ISR records the event without continuously re-entering IRQ7.
				m_pcmcia_change_ack_pending = true;
				logerror("Simon PC Card first status-change acknowledge deferred\n");
			}
			else
			{
				m_pcmcia_changed = false;
				m_pcmcia_change_ack_pending = false;
			}
		}
		if (BIT(data, 2))
			m_pcmcia_timeout = false;
		pcmcia_update_irq();
		return;
	}
	if (m_vg230_index == 0x28)
		return;
	if (m_vg230_index == 0xc0)
	{
		// Only STATE is writable.  EXT first asks the BIOS to save the display,
		// timer and peripheral context.  Stop the CPU only at its explicit
		// SUSPEND command, never at the host-key edge.
		m_vg230_regs[0xc0] = (m_vg230_regs[0xc0] & 0xfc) | (data & 3);
		if ((data & 3) == 3)
		{
			m_main_power = false;
			m_maincpu->suspend(SUSPEND_REASON_DISABLE, true);
			logerror("Simon PMU suspend pc=%05X RTC=%02u:%02u:%02u phone=%u\n", m_maincpu->pc(),
				m_vg230_regs[0x72], m_vg230_regs[0x71], m_vg230_regs[0x70], m_phone_power);
		}
		return;
	}
	if (m_vg230_index == 0xc1 || m_vg230_index == 0xcb)
		return; // read-only supply/output pins
	if (m_vg230_index == 0xda)
		data &= 1; // PMUREF is read-only, synchronous restore has completed
	if (m_vg230_index >= 0x20 && m_vg230_index <= 0x2e && m_vg230_index != 0x23 && m_vg230_index != 0x29 && m_vg230_regs[m_vg230_index] != data)
		logerror("VG230 PC Card register %02X: %02X -> %02X\n", m_vg230_index, m_vg230_regs[m_vg230_index], data);
	if (m_vg230_index >= 0xe0 && m_vg230_index <= 0xe2 && m_vg230_regs[m_vg230_index] != data)
		logerror("VG230 enhancement register %02X: %02X -> %02X\n", m_vg230_index, m_vg230_regs[m_vg230_index], data);
	m_vg230_regs[m_vg230_index] = data;
	if (m_vg230_index == 0x01)
		update_cpu_clock();
	if (m_vg230_index == 0x20 || m_vg230_index == 0x23)
		pcmcia_update_irq();
	if (m_vg230_index == 0xcf)
	{
		logerror("VG230 LCD timer set to %u minute(s)\n", data & 0x0f);
	}
}

void ibmsimon_state::update_cpu_clock()
{
	// VG230 BCG register 01h selects CPUOSC/2, /3, /4, /6 or /8.
	// Reset is /4; Simon's firmware may later select a faster operating rate.
	static constexpr unsigned divisors[8] = { 2, 3, 4, 6, 8, 8, 8, 8 };
	const u8 mode = m_vg230_regs[0x01];
	const u32 oscillator = BIT(mode, 4) ? 28'636'360 : 32'215'905;
	m_maincpu->set_unscaled_clock(oscillator / divisors[mode >> 5]);
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
	{
		m_map_high[page] = data;
		const unsigned type = (data >> 4) & 0x07;
		if (type >= 4 && m_pcmcia_map_trace_count < 512)
		{
			const u32 card_base = (u32(data & 0x0f) << 22) | (u32(m_map_low[page]) << 14);
			logerror("VG230 PC Card map page=%02X host=%05X DTYP=%u card=%07X high=%02X low=%02X E1=%02X CTL0=%02X\n",
				page, page << 14, type, card_base, data, m_map_low[page], m_vg230_regs[0xe0], m_vg230_regs[0x21]);
			++m_pcmcia_map_trace_count;
		}
	}
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
		// Bit 3 follows vertical retrace; bit 0 reports the visible interval.
		// Award's POST waits for both edges and treats a constant bit as a
		// display failure, producing a spurious 1-2 diagnostic beep code.
		return m_screen->vblank() ? 0x08 : 0x01;
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

void ibmsimon_state::dos_keyboard_put(u8 data)
{
	if (!m_main_power)
		return;
	// Simon has no built-in PC keyboard, but its ROM-DOS console consumes the
	// standard BIOS keyboard ring buffer.  Expose the host keyboard only while
	// the LCD is in text mode so Navigator remains exclusively pen-driven.
	if (BIT(m_cga_mode, 1))
		return;

	address_space &memory = m_maincpu->space(AS_PROGRAM);
	const u16 head = memory.read_word(0x041a);
	const u16 tail = memory.read_word(0x041c);
	if (head < 0x1e || head > 0x3c || (head & 1) ||
		tail < 0x1e || tail > 0x3c || (tail & 1))
		return;

	const u16 next_tail = (tail == 0x3c) ? 0x1e : tail + 2;
	if (next_tail == head)
		return;

	u8 scan = 0;
	if (data == 0x0d) scan = 0x1c;
	else if (data == 0x08) scan = 0x0e;
	else if (data == 0x1b) scan = 0x01;
	else if (data == 0x09) scan = 0x0f;
	memory.write_word(0x0400 + tail, (u16(scan) << 8) | data);
	memory.write_word(0x041c, next_tail);
}

void ibmsimon_state::ppi_b_w(u8 data)
{
	m_ppi_b = data;
	m_mb->m_pit8253->write_gate2(BIT(data, 0));
	m_mb->pc_speaker_set_spkrdata(BIT(data, 1));
}

u8 ibmsimon_state::ppi_c_r()
{
	// VG230 PPIC (62h): bits 7-6 are inactive parity-error inputs, bit 5
	// exposes timer output 2 and bit 4 is the inverted physical SPKR level.
	u8 data = m_mb->pit_out2() ? 0x20 : 0x00;
	if (!(BIT(m_ppi_b, 1) && m_mb->pit_out2()))
		data |= 0x10;

	// PB3 selects which bank of the PC-compatible configuration switches is
	// visible.  SW1-SW4 are hardwired high by the VG230; SW5-SW8 come from
	// the programmable switch field in Keyboard Mode register 08h.
	data |= BIT(m_ppi_b, 3) ? (m_vg230_regs[0x08] >> 4) : 0x0f;
	return data;
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
	const bool irq = (m_sio1_rx_ready && BIT(m_sio1_regs[1], 0))
		|| (m_sio1_thre_irq && BIT(m_sio1_regs[1], 1));
	m_mb->m_pic8259->ir4_w(irq);
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::sio1_tx_done)
{
	// BIOS queues the final digit-frame trailer after starting transmission.
	// An always-empty LSR and missing THRE interrupt stranded that trailer
	// until an unrelated incoming status happened to invoke the RF ISR.
	m_sio1_tx_empty = true;
	m_sio1_thre_irq = true;
	sio1_update_irq();
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::sio1_rx_irq)
{
	// COM1 is connected to the RF deck.  Keep consecutive three-wire status
	// bytes as separate edges for the XT PIC and the BIOS ring buffer ISR.
	m_mb->m_pic8259->ir4_w(0);
	m_sio1_rx_ready = m_sio1_rx_count != 0;
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

void ibmsimon_state::modem_result(u8 numeric, std::string_view verbose)
{
	if (m_modem_verbose)
	{
		modem_response("\r\n");
		modem_response(verbose);
		modem_response("\r\n");
	}
	else
	{
		const std::string result = std::to_string(numeric);
		modem_response(result);
		modem_response("\r");
	}
}

void ibmsimon_state::cellular_link_output(u8 data)
{
	static constexpr char hex[] = "0123456789ABCDEF";
	const char message[] = { 'T', ' ', hex[data >> 4], hex[data & 0x0f], '\n' };
	cellular_link_text(std::string_view(message, sizeof(message)));
}

void ibmsimon_state::cellular_link_text(std::string_view text)
{
	// A bitbanger output(byte) is a separate TCP send with TCP_NODELAY.  Send
	// complete frames instead, particularly the 8 kHz PCM bearer, so speech
	// does not consume thousands of host system calls per emulated second.
	if (m_cellular_link->exists())
		m_cellular_link->fwrite(text.data(), text.size());
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
	// Legacy addressed handset keys use 60xx words.  Production PHONE.EXE
	// uses a framed dial string instead; 70xx controls earpiece volume and
	// must never be dispatched here as SEND/END or as keypad DTMF.
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
		if (m_phone_call)
		{
			// AMPS carries post-connect digits as ordinary dual-tone audio on the
			// analogue voice channel.  Generate 120 ms of in-band DTMF here rather
			// than sending a private key event to the switch.  The switch therefore
			// has to recover the digit from exactly the PCM heard by the far end.
			m_dtmf_digit = digit;
			m_dtmf_samples = 960; // 120 ms at 8 kHz
			m_dtmf_low_phase = 0;
			m_dtmf_high_phase = 0;
		}
		else if (!m_call_request_sent && !m_phone_ring && m_rf_dial_size < m_rf_dial_digits.size())
			m_rf_dial_digits[m_rf_dial_size++] = u8(digit);
		return;
	}

	if (command == 0x13) // SEND (mobile-originated call)
	{
		// Repeated legacy SEND must not originate another call on an already
		// connected or pending voice channel.
		if (m_phone_call)
		{
			// This is a handset key word, not a state query.  Conversation remains
			// 6Bh; PHONE.EXE verifies it separately with the ADh/AEh audit below.
			return;
		}

		if (m_rf_dial_size && m_phone_power && !m_phone_call && !m_phone_ring && !m_call_request_sent)
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
		m_network_busy_audio = false;
		m_voice_rx_head = m_voice_rx_tail = m_voice_rx_count = 0;
		m_voice_rx_started = false;
		m_rf_call_state = 0x64;
		m_answer_request_sent = false;
		m_rf_answer_prefix = false;
		m_call_request_sent = false;
		m_rf_data_setup = false;
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

void ibmsimon_state::cellular_start_ringing(bool pager)
{
	if (!m_phone_power)
	{
		popmessage("Virtual 1G call not delivered: Simon phone is off");
		return;
	}
	m_phone_call = false;
	m_phone_ring = true;
	m_phone_auto_answering = pager;
	m_phone_muted = false;
	m_phone_ring_pulses = 0;
	m_network_busy_audio = false;
	m_rf_call_state = 0x6a;
	m_answer_request_sent = false;
	m_rf_answer_prefix = false;
	if (pager)
	{
		// Numeric paging is an unattended RF-deck transaction.  It must not
		// assert A7/6A, which would make PHONE.EXE display an ordinary incoming
		// call and run the audible ringer.  Accept Pages is the authorization;
		// the deck silently takes the assigned voice path so its DTMF decoder can
		// consume the network callback digits.
		m_phone_ring = false;
		m_rf_call_state = 0x6e;
		m_answer_request_sent = true;
		m_pager_capture_started = false;
		m_pager_capture_size = 0;
		m_modem_dtmf_count = 0;
		m_modem_dtmf_candidate = 0;
		m_modem_dtmf_stable = 0;
		m_modem_dtmf_active = 0;
		cellular_link_text("A\n");
		return;
	}
	pmu_ring(); // the independent RF-deck RI input can wake the sleeping PDA
	// The cell site has already completed page response and voice-channel
	// assignment before it issues R.  A7h is the incoming-call latch used both
	// by PHONE.EXE for Answer and by NAV for its local audible cadence.  6Ah
	// reports Wait For Answer, but does not itself trigger the native ringer.
	// Keep these as distinct paced RF bytes; changing only their relative
	// delay cannot remove NAV's synchronous ring/input wait on every UI phase.
	sio1_rx_byte(0x63); // page/control-channel event retained for BIOS history
	sio1_rx_byte(0xa7); // persistent incoming-call / native alert indication
	rf_schedule_event(2, 1); // wait-for-answer/alert on next 50 ms control tick
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
			std::string caller("unknown");
			if (m_cellular_link_line_size > 2 && m_cellular_link_line[1] == ' ')
				caller.assign(reinterpret_cast<const char *>(&m_cellular_link_line[2]), m_cellular_link_line_size - 2);
			const bool pager = caller.ends_with(" P");
			if (pager)
				caller.resize(caller.size() - 2);
			if (pager && !m_phone_accept_pages)
				cellular_link_text("H\n");
			else
			{
				cellular_start_ringing(pager);
				if (!pager)
					popmessage("Virtual 1G incoming call from %s\nRF state: Wait For Answer (6A)", caller.c_str());
			}
		}
	}
	else if (command == 'C')
	{
		logerror("Simon switch C received ring=%d call=%d phase=%u ticks=%u\n",
			m_phone_ring, m_phone_call, m_rf_event_phase, m_rf_event_ticks);
		if (m_phone_auto_answering)
		{
			// The switch confirms the RF deck's private pager bearer with the same
			// C message used for a handset call.  It is not a Conversation state as
			// far as PHONE.EXE is concerned: sending 6Bh here briefly opens In Call,
			// and that redraw can also consume/hide the following unread-page latch.
			// Keep only the internal bearer alive for the DTMF decoder until H.
			m_phone_ring = false;
			m_phone_call = false;
			m_phone_muted = false;
			m_network_busy_audio = false;
			m_voice_rx_head = m_voice_rx_tail = m_voice_rx_count = 0;
			m_voice_rx_started = false;
			m_rf_call_state = 0x6e;
			m_answer_request_sent = true;
			m_rf_answer_prefix = false;
			m_call_request_sent = false;
			m_rf_data_setup = false;
			m_rf_event_phase = 0;
			m_rf_dial_size = 0;
			return;
		}
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
		m_network_busy_audio = false;
		m_voice_rx_head = m_voice_rx_tail = m_voice_rx_count = 0;
		m_voice_rx_started = false;
		m_rf_call_state = 0x6b;
		m_answer_request_sent = false;
		m_rf_answer_prefix = false;
		m_call_request_sent = false;
		m_rf_data_setup = false;
		m_rf_event_phase = 0;
		m_rf_dial_size = 0;
		// Conversation (6Bh) clears both the RF-deck incoming latch and the
		// BIOS incoming-call flag.  Do not precede it with A8h: that byte also
		// raises BIOS event 0400h, which PHONE.EXE treats as a missed call.
		rf_schedule_event(4, 2); // conversation after the serial response gap
		if (!m_phone_auto_answering)
			popmessage("Virtual 1G call connected\nRF state: Conversation (6B)");
	}
	else if (command == 'H')
	{
		const bool was_ringing = m_phone_ring;
		const bool was_call = m_phone_call;
		const bool was_pager = m_phone_auto_answering;
		m_phone_ring = false;
		m_phone_call = false;
		m_phone_auto_answering = false;
		m_phone_muted = false;
		m_network_busy_audio = false;
		m_voice_rx_head = m_voice_rx_tail = m_voice_rx_count = 0;
		m_voice_rx_started = false;
		m_rf_call_state = 0x64;
		m_answer_request_sent = false;
		m_rf_answer_prefix = false;
		m_call_request_sent = false;
		m_rf_event_phase = 0;
		m_rf_dial_size = 0;
		// A numeric page is captured entirely inside the virtual RF deck.  PHONE.EXE
		// never saw an incoming call or a voice-channel assignment, so feeding it a
		// release/idle transition here fabricates a short "In Call" teardown screen
		// when the pager bearer closes.  Only real handset calls own host-visible
		// call state; the pager path merely drops its private bearer above.
		if (!was_pager)
		{
			if (was_ringing)
				sio1_rx_byte(0xa8); // incoming-call indication off
			if (was_call)
				sio1_rx_byte(0x81); // handset/voice channel no longer in use
			sio1_rx_byte(0x64); // idle
			popmessage("Virtual 1G call ended\nRF state: Idle (64)");
		}
	}
	else if (command == 'B')
	{
		// The called subscriber is busy.  This is not AMPS system congestion, so
		// do not raise Simon's native "cellular system is busy" dialog.  Keep the
		// caller off-hook/originating and open the downlink audio path; the AMPS
		// switch supplies the busy cadence as ordinary voice samples until END.
		m_phone_ring = false;
		m_phone_call = false;
		m_phone_muted = false;
		m_network_busy_audio = true;
		m_voice_rx_head = m_voice_rx_tail = m_voice_rx_count = 0;
		m_voice_rx_started = false;
		m_call_request_sent = true;
		m_answer_request_sent = false;
		m_rf_call_state = 0x65;
		m_rf_event_phase = 0;
		sio1_rx_byte(0x65);
		popmessage("Virtual 1G called subscriber busy\nBusy tone active - press END");
	}
	else if (command == 'M' && m_cellular_link_line_size >= 3 && m_cellular_link_line[1] == ' ')
	{
		const u8 result = std::toupper(m_cellular_link_line[2]);
		if (result == 'C')
		{
			// The stock Mail/Fax path originates the cellular leg through the
			// INT 6Bh RF-deck interface, then consumes carrier and payload on the
			// separate Cirrus COM2 modem.  Assert both sides of that hardware
			// hand-off: 6Bh completes the cellular call, CONNECT wakes RCONNECT.
			if (m_rf_data_setup)
			{
				m_phone_call = true;
				m_phone_ring = false;
				m_rf_call_state = 0x6b;
				m_call_request_sent = false;
				m_rf_data_setup = false;
				sio1_rx_byte(0x6b);
				// RCONNECT now initializes COM2 with its own Hayes command
				// sequence.  Do not assert modem carrier here: doing so turns
				// AT&F/ATS... into mail payload before the modem can answer OK.
				m_modem_dialing = false;
				m_modem_online = false;
				m_modem_escape_count = 0;
				return;
			}
			m_modem_dialing = false;
			m_modem_online = true;
			m_modem_escape_count = 0;
			modem_result(10, "CONNECT 2400");
		}
		else if (result == 'B')
		{
			m_rf_data_setup = false;
			m_call_request_sent = false;
			m_modem_dialing = false;
			m_modem_online = false;
			modem_result(7, "BUSY");
		}
		else if (result == 'N' || result == 'H')
		{
			m_rf_data_setup = false;
			m_call_request_sent = false;
			m_phone_call = false;
			m_rf_call_state = 0x64;
			m_modem_dialing = false;
			m_modem_online = false;
			m_modem_escape_count = 0;
			modem_result(3, "NO CARRIER");
		}
		else if (result == 'X' && m_cellular_link_line_size >= 6)
		{
			auto const nibble = [] (u8 ch) -> int
			{
				ch = std::toupper(ch);
				return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
			};
			for (unsigned pos = 4; pos + 1 < m_cellular_link_line_size; )
			{
				while (pos < m_cellular_link_line_size && m_cellular_link_line[pos] == ' ')
					++pos;
				if (pos + 1 >= m_cellular_link_line_size)
					break;
				const int high = nibble(m_cellular_link_line[pos++]);
				const int low = nibble(m_cellular_link_line[pos++]);
				if (high < 0 || low < 0)
					break;
				uart_rx_byte(u8((high << 4) | low));
			}
		}
	}
	else if (command == 'Y' && m_cellular_link_line_size >= 4)
	{
		// 8 kHz unsigned PCM voice packet from the peer Simon.  Voice remains
		// outside the guest CPU, as on the analogue AMPS hardware, while the
		// RF/phone state machine still decides when the path is open.
		auto const nibble = [] (u8 ch) -> int
		{
			ch = std::toupper(ch);
			return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
		};
		for (unsigned pos = 2; pos + 1 < m_cellular_link_line_size; )
		{
			while (pos < m_cellular_link_line_size && m_cellular_link_line[pos] == ' ')
				++pos;
			if (pos + 1 >= m_cellular_link_line_size || (!m_phone_auto_answering && m_voice_rx_count >= m_voice_rx.size()))
				break;
			const int high = nibble(m_cellular_link_line[pos++]);
			const int low = nibble(m_cellular_link_line[pos++]);
			if (high < 0 || low < 0)
				break;
			const u8 sample = u8((high << 4) | low);
			// A pager's signalling audio terminates inside the RF deck.  Feed it
			// to the DTMF detector but never bridge it to Simon's earpiece DAC.
			if (!m_phone_auto_answering)
			{
				m_voice_rx[m_voice_rx_tail] = sample;
				m_voice_rx_tail = (m_voice_rx_tail + 1) % m_voice_rx.size();
				++m_voice_rx_count;
			}
			modem_voice_sample(sample);
		}
	}
	else if (command == 'G')
	{
		// A control-channel numeric page does not seize an AMPS voice channel.
		// The stock Phone Pager workflow originates pages through the pager IVR;
		// this indication is also used by the switch GUI for direct laboratory
		// delivery and deliberately leaves PHONE.EXE's call flags untouched.
		std::string page(reinterpret_cast<const char *>(m_cellular_link_line.data()), m_cellular_link_line_size);
		const std::size_t first = page.find(' ');
		const std::size_t second = first == std::string::npos ? first : page.find(' ', first + 1);
		const std::string sender = first == std::string::npos ? "unknown" : page.substr(first + 1, second - first - 1);
		const std::string message = second == std::string::npos ? "" : page.substr(second + 1);
		popmessage("AMPS pager message\nFrom: %s\n%s", sender.c_str(), message.c_str());
	}
	else if (command == 'S')
	{
		// Host switch profile: S <HOME1|HOME2|HOME3|HOME4|ROAM|ALTROAM|OFFLINE>
		// <0..6> [operator name].  AMPS exposes the system class and signal to
		// Simon; the free-form operator name remains available to the host link.
		const u8 previous_registration = m_cellular_registration;
		const u8 previous_signal = m_cellular_signal;
		const auto previous_operator = m_cellular_operator;
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
		const bool changed = previous_registration != m_cellular_registration
			|| previous_signal != m_cellular_signal || previous_operator != m_cellular_operator;
		if (m_phone_power && changed)
		{
			if (m_phone_call || m_phone_ring || m_call_request_sent || m_phone_auto_answering)
			{
				// Registration updates do not release an assigned voice channel or
				// replace a pending Answer event with the idle registration sequence.
				sio1_rx_byte(m_cellular_registration <= 5 ? 0x83 : 0x82);
				if (m_cellular_registration <= 5)
					sio1_rx_byte(0x84 + m_cellular_registration);
				sio1_rx_byte(0xa0 + m_cellular_signal);
			}
			else
				cellular_report_network();
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
		if (changed)
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
	else if (command == 'D')
	{
		// RF diagnostic profile supplied by the virtual cell site:
		// D <SID 0..9999> <channel 1..1023> [SAT 5970|6000|6030].  SAT is
		// supervised inside the RF deck because the 8 kHz subscriber PCM path
		// cannot represent the real out-of-band supervisory tone.
		std::string line(reinterpret_cast<const char *>(m_cellular_link_line.data()), m_cellular_link_line_size);
		unsigned sid = 0;
		unsigned channel = 0;
		unsigned sat = m_cellular_sat;
		const int fields = std::sscanf(line.c_str(), "D %u %u %u", &sid, &channel, &sat);
		if (fields >= 2)
		{
			m_cellular_sid = std::min<unsigned>(sid, 9999);
			m_cellular_channel = std::clamp<unsigned>(channel, 1, 1023);
			if (fields >= 3 && (sat == 5970 || sat == 6000 || sat == 6030))
				m_cellular_sat = sat;
		}
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

void ibmsimon_state::rf_status_response(u8 query, u16 value)
{
	// Mitsubishi's diagnostic reply is neither ASCII nor a binary integer.
	// The deck first echoes the query, then sends one decimal digit per byte.
	// B0h..B9h are continuation digits and F0h..F9h terminate the value.  This
	// is the exact framing PHONE.EXE's Status Monitor parser accepts.
	const std::string digits = std::to_string(value);
	sio1_rx_byte(query);
	for (std::size_t index = 0; index < digits.size(); ++index)
	{
		const u8 digit = u8(digits[index] - '0');
		sio1_rx_byte((index + 1 == digits.size() ? 0xf0 : 0xb0) | digit);
	}
}

void ibmsimon_state::modem_voice_sample(u8 sample)
{
	// Simon routes the received cellular audio through the CL-MD1224 voice
	// modem when Phone Pager is enabled.  That modem reports a detected DTMF
	// key to the host as the IS-101 <DLE><key> pair.  Previously the virtual
	// bearer only drove the speaker DAC, so PHONE.EXE could hear a page but
	// could never record its digits.
	if (!m_modem_voice && !m_phone_auto_answering)
	{
		m_modem_dtmf_count = 0;
		m_modem_dtmf_candidate = 0;
		m_modem_dtmf_stable = 0;
		m_modem_dtmf_active = 0;
		return;
	}

	m_modem_dtmf_window[m_modem_dtmf_count++] = s16(sample) - 128;
	if (m_modem_dtmf_count < m_modem_dtmf_window.size())
		return;
	m_modem_dtmf_count = 0;

	auto const power = [this] (double frequency)
	{
		const double omega = 6.28318530717958647692 * frequency / 8000.0;
		const double coefficient = 2.0 * std::cos(omega);
		double q1 = 0.0;
		double q2 = 0.0;
		for (s16 value : m_modem_dtmf_window)
		{
			const double q0 = coefficient * q1 - q2 + double(value) / 128.0;
			q2 = q1;
			q1 = q0;
		}
		return q1 * q1 + q2 * q2 - coefficient * q1 * q2;
	};

	static constexpr std::array<double, 4> low_frequency{ 697.0, 770.0, 852.0, 941.0 };
	static constexpr std::array<double, 3> high_frequency{ 1209.0, 1336.0, 1477.0 };
	static constexpr char keys[4][3]{ { '1','2','3' }, { '4','5','6' }, { '7','8','9' }, { '*','0','#' } };
	std::array<double, 4> low_power{};
	std::array<double, 3> high_power{};
	for (unsigned index = 0; index < low_power.size(); ++index)
		low_power[index] = power(low_frequency[index]);
	for (unsigned index = 0; index < high_power.size(); ++index)
		high_power[index] = power(high_frequency[index]);
	const unsigned low = std::max_element(low_power.begin(), low_power.end()) - low_power.begin();
	const unsigned high = std::max_element(high_power.begin(), high_power.end()) - high_power.begin();
	double low_second = 0.0;
	double high_second = 0.0;
	for (unsigned index = 0; index < low_power.size(); ++index)
		if (index != low) low_second = std::max(low_second, low_power[index]);
	for (unsigned index = 0; index < high_power.size(); ++index)
		if (index != high) high_second = std::max(high_second, high_power[index]);
	const bool dominant = low_power[low] > low_second * 2.5 && high_power[high] > high_second * 2.5;
	const char found = dominant && low_power[low] + high_power[high] > 18.0 ? keys[low][high] : 0;

	if (found == m_modem_dtmf_candidate)
		m_modem_dtmf_stable = std::min<u8>(m_modem_dtmf_stable + 1, 3);
	else
	{
		m_modem_dtmf_candidate = found;
		m_modem_dtmf_stable = 1;
	}
	if (m_modem_dtmf_stable < 2)
		return;
	if (!found)
	{
		m_modem_dtmf_active = 0;
		return;
	}
	if (m_modem_dtmf_active == found)
		return;
	m_modem_dtmf_active = found;
	logerror("Simon %s DTMF: %c\n", m_phone_auto_answering ? "RF pager" : "voice modem", found);
	if (m_phone_auto_answering)
	{
		if (found == '#')
		{
			if (!m_pager_capture_started)
			{
				m_pager_capture_started = true;
				m_pager_capture_size = 0;
			}
			else
			{
				// The RF deck retains the newest nine callback numbers.  Slot 1
				// is the top row of PHONE.EXE's Phone Pager screen.
				for (unsigned slot = 8; slot != 0; --slot)
				{
					m_pager_lengths[slot] = m_pager_lengths[slot - 1];
					std::copy_n(&m_pager_pages[(slot - 1) * 33], 33, &m_pager_pages[slot * 33]);
				}
				m_pager_lengths[0] = m_pager_capture_size;
				std::fill_n(&m_pager_pages[0], 33, 0);
				std::copy_n(m_pager_capture.begin(), m_pager_capture_size, m_pager_pages.begin());
				m_pager_capture_started = false;
				m_pager_beep_samples = 1600;
				m_pager_beep_phase = 0;
				// Report the completed numeric page separately from call-state bytes.
				// 1Eh is the RF deck's native "numeric page waiting" indication.  The
				// BIOS expands it into status bits 0800h and 2000h; PHONE.EXE watches
				// 0800h to draw the heavy unread border around Phone Pager.  AA/AB are
				// unrelated 0200h events and do not drive that control.
				m_pager_unread = true;
				sio1_rx_byte(0x1e);
				logerror("Simon RF pager stored slot 1: %.*s\n", m_pager_capture_size,
					reinterpret_cast<const char *>(m_pager_capture.data()));
			}
		}
		else if (m_pager_capture_started && found >= '0' && found <= '9' && m_pager_capture_size < m_pager_capture.size() - 1)
		{
			m_pager_capture[m_pager_capture_size++] = u8(found);
		}
		return;
	}
	uart_rx_byte(0x10); // IS-101 DLE event introducer
	uart_rx_byte(u8(found));
}

void ibmsimon_state::modem_execute_command()
{
	std::string command(reinterpret_cast<const char *>(m_modem_command.data()), m_modem_command_size);
	m_modem_command_size = 0;
	std::transform(command.begin(), command.end(), command.begin(), [](unsigned char ch) { return std::toupper(ch); });
	logerror("Simon modem command: %s\n", command.c_str());

	// STORM's system diagnostic defensively transmits the Hayes escape guard
	// sequence before every reset, even when the modem is already in command
	// mode.  The Cirrus part silently consumes it and parses the following ATZ;
	// treating the pluses as command text makes Simon report a modem failure.
	while (command.starts_with("+++"))
		command.erase(0, 3);
	auto const information = [this] (std::string_view text)
	{
		if (m_modem_verbose)
		{
			modem_response("\r\n");
			modem_response(text);
			modem_response("\r\n");
		}
		else
		{
			modem_response(text);
			modem_response("\r");
		}
	};

	if (!command.starts_with("AT"))
	{
		modem_result(4, "ERROR");
		return;
	}

	if (command == "ATZ")
	{
		m_modem_echo = true;
		m_modem_verbose = true;
		if (m_modem_online || m_modem_dialing)
			cellular_link_text("M H\n");
		m_modem_online = false;
		m_modem_dialing = false;
		m_modem_voice = false;
		m_modem_escape_count = 0;
		m_modem_fax_class = 0;
	}
	else if (command.find("E0") != std::string::npos)
	{
		m_modem_echo = false;
	}
	else if (command.find("E1") != std::string::npos)
	{
		m_modem_echo = true;
	}
	if (command.find("V0") != std::string::npos)
		m_modem_verbose = false;
	else if (command.find("V1") != std::string::npos)
		m_modem_verbose = true;

	if (command.find("#VCL=1") != std::string::npos)
	{
		// CL-MD1224 online voice-command mode.  This does not switch the RF
		// deck itself on; it connects the modem's detector to the existing
		// cellular audio path so PHONE.EXE can receive DLE DTMF events.
		m_modem_voice = true;
		m_modem_dtmf_count = 0;
		m_modem_dtmf_candidate = 0;
		m_modem_dtmf_stable = 0;
		m_modem_dtmf_active = 0;
	}

	// On the CL-MD1224/1624 firmware used by Simon, I4 is the fitted SRAM
	// configuration query (the meaning was changed/reserved on later FastPath
	// parts).  STORM requires configuration bits 3 and 4 in the information
	// line before the result code to confirm that both 16 KiB SRAM banks are
	// present (18h, printed as decimal 24 while V0 is active).
	if (command == "ATI0")
	{
		information("1224");
		modem_result(0, "OK");
		return;
	}
	if (command == "ATI1")
	{
		information("000");
		modem_result(0, "OK");
		return;
	}
	if (command == "ATI2")
	{
		modem_result(0, "OK");
		return;
	}
	if (command == "ATI3")
	{
		information("CL-MD1224");
		modem_result(0, "OK");
		return;
	}
	if (command == "ATI4")
	{
		information("24");
		modem_result(0, "OK");
		return;
	}
	if (command == "ATI5")
	{
		information("32K SRAM PRESENT");
		modem_result(0, "OK");
		return;
	}
	if (command == "ATI6")
	{
		information("US");
		modem_result(0, "OK");
		return;
	}
	if (command == "ATI7")
	{
		information("Cirrus Logic, Inc.");
		modem_result(0, "OK");
		return;
	}
	if (command.find("+FCLASS=?") != std::string::npos)
	{
		information("0,1");
		modem_result(0, "OK");
		return;
	}
	if (command.find("+FCLASS?") != std::string::npos)
	{
		information(std::to_string(m_modem_fax_class));
		modem_result(0, "OK");
		return;
	}
	if (const std::size_t fax_class = command.find("+FCLASS="); fax_class != std::string::npos)
	{
		if (fax_class + 8 >= command.size())
		{
			modem_result(4, "ERROR");
			return;
		}
		const char value = command[fax_class + 8];
		if (value != '0' && value != '1')
		{
			modem_result(4, "ERROR");
			return;
		}
		m_modem_fax_class = value - '0';
	}

	if (command.starts_with("ATD") || command.find("DT") != std::string::npos)
	{
		std::size_t dial = command.starts_with("ATD") ? 3 : command.find("DT") + 2;
		if (dial < command.size() && (command[dial] == 'T' || command[dial] == 'P'))
			++dial;
		std::string message("M D ");
		for (std::size_t i = dial; i < command.size(); ++i)
		{
			const char ch = command[i];
			if ((ch < '0' || ch > '9') && ch != '*' && ch != '#')
				break;
			message.push_back(ch);
		}
		if (message.size() > 4)
		{
			message.push_back('\n');
			cellular_link_text(message);
			m_modem_dialing = true;
			m_modem_online = false;
			return; // CONNECT/BUSY/NO CARRIER is asynchronous from the switch.
		}
		// Simon's cellular Mail/Fax stack originates the telephone number over
		// INT 6Bh first.  Its bare ATD then asks the CL-MD1224 to train on that
		// already-established analogue bearer; it is not a request for a second
		// telephone dial.  Assert modem carrier only at this final stage.
		if (command == "ATD" && m_phone_call)
		{
			m_modem_dialing = false;
			m_modem_online = true;
			m_modem_escape_count = 0;
			modem_result(10, "CONNECT 2400");
			return;
		}
		modem_result(6, "NO DIALTONE");
		return;
	}
	else if (command == "ATH" || command.starts_with("ATH0"))
	{
		if (m_modem_online || m_modem_dialing)
			cellular_link_text("M H\n");
		m_modem_online = false;
		m_modem_dialing = false;
		m_modem_escape_count = 0;
	}
	else if (command.find("#VCL=0") != std::string::npos)
	{
		m_modem_voice = false;
		m_modem_dtmf_count = 0;
		m_modem_dtmf_candidate = 0;
		m_modem_dtmf_stable = 0;
		m_modem_dtmf_active = 0;
	}

	// The RF deck uses a Hayes-compatible command channel with Cirrus Logic
	// voice/cellular extensions.  Unsupported setters are deliberately
	// accepted here so the command log can drive progressively deeper HLE.
	modem_result(0, "OK");
}

void ibmsimon_state::modem_receive_byte(u8 data)
{
	if (m_modem_online)
	{
		const u16 ss = m_maincpu->state_int(NEC_SS);
		const u16 sp = m_maincpu->state_int(NEC_SP);
		auto &space = m_maincpu->space(AS_PROGRAM);
		const u32 stack = (u32(ss) << 4) + sp;
		logerror("Simon modem data TX pc=%05X cs:ip=%04X:%04X byte=%02X stack=%04X,%04X,%04X,%04X\n",
			m_maincpu->pc(), u16(m_maincpu->state_int(NEC_PS)), u16(m_maincpu->state_int(NEC_PC)), data,
			space.read_word(stack), space.read_word(stack + 2), space.read_word(stack + 4), space.read_word(stack + 6));
		// Hayes guard times are intentionally relaxed for the virtual bearer.
		// Three consecutive plus characters return to command mode; all other
		// bytes are transported losslessly to the switch as hexadecimal octets.
		if (data == '+')
		{
			if (++m_modem_escape_count == 3)
			{
				m_modem_online = false;
				m_modem_escape_count = 0;
				modem_result(0, "OK");
			}
			return;
		}
		m_modem_escape_count = 0;
		static constexpr char hex[] = "0123456789ABCDEF";
		const char message[] = { 'M', ' ', 'X', ' ', hex[data >> 4], hex[data & 0x0f], '\n' };
		cellular_link_text(std::string_view(message, sizeof(message)));
		return;
	}

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
	if (m_rf_tx_address == 0x70)
	{
		// PHONE 55F36, 5C8CF and 5C940 send 70h followed by 10h + volume
		// (1..7).  Mistaking volume 3 for SEND delayed origination by eight
		// seconds; volume 7 was incorrectly treated as END.
		m_rf_tx_address = 0;
		if (data >= 0x11 && data <= 0x17)
			m_phone_volume = data - 0x10;
		return;
	}

	// Phone Pager's Erase mode selects one of nine rows.  PHONE.EXE sends
	// B1h..B9h and waits for the matching 91h..99h completion before removing
	// that row.  Compact the deck slots as the native UI compacts its list.
	if (!m_rf_tx_address && data >= 0xb1 && data <= 0xb9)
	{
		const unsigned erased = data - 0xb1;
		for (unsigned slot = erased; slot < 8; ++slot)
		{
			m_pager_lengths[slot] = m_pager_lengths[slot + 1];
			std::copy_n(&m_pager_pages[(slot + 1) * 33], 33, &m_pager_pages[slot * 33]);
		}
		m_pager_lengths[8] = 0;
		std::fill_n(&m_pager_pages[8 * 33], 33, 0);
		if (!m_pager_lengths[0])
			m_pager_unread = false;
		sio1_rx_byte(data - 0x20);
		logerror("Simon RF pager erased slot %u\n", erased + 1);
		return;
	}

	// PHONE.EXE queries the nine numeric-pager slots with 01h..09h.  A slot
	// reply is its echoed index, the ASCII callback digits, then 22h.  Empty
	// slots use the same framing without digits, avoiding a two-second timeout.
	if (!m_rf_tx_address && data >= 0x01 && data <= 0x09)
	{
		const unsigned slot = data - 1;
		sio1_rx_byte(data);
		for (unsigned index = 0; index < m_pager_lengths[slot]; ++index)
			sio1_rx_byte(m_pager_pages[slot * 33 + index]);
		sio1_rx_byte(0x22);
		// Reading slot 1 acknowledges the notification, but does not erase the
		// callback number.  PHONE.EXE/BIOS owns consumption of the native 1Eh
		// status; do not inject ABh here (ABh only clears the unrelated 0200h
		// event latch).
		if (slot == 0 && m_pager_unread)
			m_pager_unread = false;
		return;
	}

	// PHONE.EXE controls the Mitsubishi deck with its native power commands,
	// not with the reset pulse on the shared board latch.  7Fh completes the
	// power-up sequence; 8Ch removes phone power.  The latter also occurs at
	// boot to ensure the deck starts off.  Tracking these commands keeps the
	// amber indicator independent from both BIOS POST and main-unit suspend.
	if (data == 0x7f)
	{
		if (!m_phone_power)
		{
			m_phone_power = true;
			rf_schedule_event(6, 20);
		}
		return;
	}
	if (data == 0x8c)
	{
		m_phone_power = false;
		m_phone_call = false;
		m_phone_ring = false;
		m_phone_auto_answering = false;
		m_phone_muted = false;
		m_network_busy_audio = false;
		m_voice_rx_head = m_voice_rx_tail = m_voice_rx_count = 0;
		m_voice_rx_started = false;
		m_call_request_sent = false;
		m_rf_data_setup = false;
		m_rf_dial_pending = false;
		m_answer_request_sent = false;
		m_rf_event_phase = 0;
		m_rf_event_ticks = 0;
		return;
	}

	// PHONE.EXE first queries the RF deck's unattended-answer options, then
	// writes the changed values when Phone Settings is accepted.  The query
	// and value bytes are deliberately asymmetric: A6 expects 7C/7D,
	// A5 expects 7A/7B, 1E expects 1C/1D, and 0C expects 0A/0B.  Treating a
	// query as a setter (or merely echoing it) makes PHONE.EXE time out and
	// leaves its native pager state disabled even though the checkbox is drawn.
	if (!m_rf_tx_address && data == 0xa6)
	{
		sio1_rx_byte(m_phone_auto_answer ? 0x7c : 0x7d);
		return;
	}
	if (!m_rf_tx_address && data == 0xa5)
	{
		sio1_rx_byte(0x7b); // auxiliary auto-answer option: disabled
		return;
	}
	if (!m_rf_tx_address && data == 0x1e)
	{
		sio1_rx_byte(m_phone_accept_pages ? 0x1c : 0x1d);
		return;
	}
	if (!m_rf_tx_address && data == 0x0c)
	{
		sio1_rx_byte(0x0b); // auxiliary alert option: disabled
		return;
	}
	if (!m_rf_tx_address && (data == 0x1c || data == 0x1d))
	{
		m_phone_accept_pages = data == 0x1c;
		return;
	}
	if (!m_rf_tx_address && (data == 0x7c || data == 0x7d))
	{
		m_phone_auto_answer = data == 0x7c;
		return;
	}

	// PHONE.EXE's Status Monitor polls three deck diagnostics.  Each response
	// uses the deck's echoed-command plus BCD-like digit framing, not a raw
	// integer: 25h = received RSSI, 28h = SID, 29h = active RF channel.
	if (data == 0x25)
	{
		static constexpr u16 rssi_by_signal[] = { 0, 42, 84, 126, 168, 210, 255 };
		rf_status_response(data, rssi_by_signal[std::min<u8>(m_cellular_signal, 6)]);
		return;
	}
	if (data == 0x28)
	{
		rf_status_response(data, m_cellular_sid);
		return;
	}
	if (data == 0x29)
	{
		rf_status_response(data, m_cellular_channel);
		return;
	}
	if (m_phone_ring || m_phone_call)
		logerror("Simon RF transmit %02X ring=%d call=%d phase=%u ticks=%u\n",
			data, m_phone_ring, m_phone_call, m_rf_event_phase, m_rf_event_ticks);

	// Production PHONE.EXE rejects an incoming call and ends an established
	// call through INT 15h/A0h, AL=03h.  The BIOS serializes that operation as
	// the single RF-deck command 88h, not the 70h,17h maximum-volume word.
	if (data == 0x88 && (m_phone_ring || m_phone_call || m_call_request_sent))
	{
		const bool was_ringing = m_phone_ring;
		const bool was_call = m_phone_call;
		m_phone_ring = false;
		m_phone_call = false;
		m_phone_muted = false;
		m_network_busy_audio = false;
		m_voice_rx_head = m_voice_rx_tail = m_voice_rx_count = 0;
		m_voice_rx_started = false;
		m_rf_call_state = 0x64;
		m_answer_request_sent = false;
		m_rf_answer_prefix = false;
		m_call_request_sent = false;
		m_rf_data_setup = false;
		m_rf_dial_size = 0;
		m_rf_dial_pending = false;
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

	// RCONNECT (the stock cc:Mail Remote transport) uses the RF-deck BIOS
	// interface before it opens the Cirrus data modem.  Its idle 88h is a
	// defensive on-hook/reset, followed by AEh readiness polls and an
	// 18h/digits/89h cellular dial frame.  PHONE.EXE uses 88h only while a
	// voice transaction is active (handled above), so the idle form safely
	// identifies this data-call setup path.
	if (data == 0x88)
	{
		m_rf_data_setup = true;
		m_rf_dial_frame = false;
		m_rf_dial_pending = false;
		m_rf_dial_size = 0;
		sio1_rx_byte(0x88);
		return;
	}

	// 89h,1Ah/1Bh terminates PHONE.EXE's dial-string frame.  It is deliberately
	// also the native Answer request while the deck is in Wait For Answer.  The
	// call-state context disambiguates it from normal mobile-originated dialing.
	// BIOS INT15/A002 selects 1Ah when DX=1, otherwise 1Bh (FB646..FB654).
	if (m_phone_ring && data == 0x89)
	{
		m_rf_answer_prefix = true;
		return;
	}
	if (m_rf_answer_prefix)
	{
		m_rf_answer_prefix = false;
		if (m_phone_ring && (data == 0x1a || data == 0x1b))
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

	// INT15/A001 serializes a complete originate request as 18h, ASCII
	// digits, 89h, then 1Ah/1Bh.  Start when that frame is complete, without
	// waiting for the unrelated volume initialization in the in-call window.
	if (m_rf_dial_pending && (data == 0x1a || data == 0x1b))
	{
		m_rf_dial_pending = false;
		rf_handset_command(0x13);
		return;
	}
	if (data == 0x18)
	{
		m_rf_dial_frame = true;
		m_rf_dial_pending = false;
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
			m_rf_dial_pending = data == 0x89 && !m_rf_data_setup && m_rf_dial_size;
			if (data == 0x89 && m_rf_data_setup && m_rf_dial_size)
			{
				std::string message("M D ");
				message.append(reinterpret_cast<const char *>(m_rf_dial_digits.data()), m_rf_dial_size);
				message.push_back('\n');
				cellular_link_text(message);
				m_modem_dialing = true;
				m_modem_online = false;
				m_call_request_sent = true;
			}
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
		// A rejected outgoing attempt remains a live PHONE.EXE transaction until
		// the user acknowledges it with END.  Restating Idle during that interval
		// erases the deck's busy termination state and makes the UI alternate
		// between idle and in-call screens before its dialog can be presented.
		sio1_rx_byte(m_phone_call || m_phone_ring || m_call_request_sent ? m_rf_call_state : 0x64);
		return;
	}

	// The ADh/AEh audit runs on the standby dial pad as well as during calls.
	// AEh acknowledges deck readiness, not an established voice channel.  Only
	// answering it during a call made standby repeatedly time out, pulse reset
	// and block the UI thread, delaying both short touches and the Answer redraw.
	if (!m_rf_tx_address && data == 0xae)
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

	// The power-on POST acknowledges with 21h, but the 24h AUDIT command returns
	// a diagnostic status byte.  5Bh is healthy; 5Ch through 5Fh report ESN,
	// NAM, synthesizer-lock and power faults respectively (as documented by
	// Simon's own diagnostics).  Returning the POST ACK here makes Test Phone
	// reject an otherwise working deck as an unexpected 12xx status.
	if (data == 0x24)
	{
		sio1_rx_byte(0x24);
		sio1_rx_byte(0x5b);
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
		if (address == 0x60 && data != 0x7f && data != 0x6b)
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
		if (!m_sio1_rx_ready)
			return 0xff;
		const u8 data = m_sio1_rx[m_sio1_rx_head];
		m_sio1_rx_head = (m_sio1_rx_head + 1) % m_sio1_rx.size();
		--m_sio1_rx_count;
		m_sio1_rx_ready = false;
		m_mb->m_pic8259->ir4_w(0);
		if (m_sio1_rx_count)
			m_sio1_rx_irq_timer->adjust(attotime::from_ticks(std::max<u16>(m_sio1_divisor, 1) * 160, 1'843'200));
		sio1_update_irq();
		return data;
	}
	// Queued wire bytes have not reached the UART receive register until
	// their character timer expires.  Polling LSR or re-enabling IER must not
	// bypass that timer and deliver several protocol fields at the same instant.
	if (offset == 2)
	{
		if (m_sio1_rx_ready && BIT(m_sio1_regs[1], 0)) return 0x04;
		if (m_sio1_thre_irq && BIT(m_sio1_regs[1], 1))
		{
			m_sio1_thre_irq = false; // reading a THRE IIR acknowledges this source
			sio1_update_irq();
			return 0x02;
		}
		return 0x01;
	}
	if (offset == 5) return (m_sio1_tx_empty ? 0x60 : 0x00) | (m_sio1_rx_ready ? 0x01 : 0x00);
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
		m_sio1_tx_empty = false;
		m_sio1_thre_irq = false;
		rf_receive_byte(data);
		m_sio1_tx_timer->adjust(attotime::from_ticks(std::max<u16>(m_sio1_divisor, 1) * 160, 1'843'200));
		sio1_update_irq();
	}
	else if (offset == 1)
	{
		m_sio1_regs[offset] = data & 0x0f;
		if (BIT(data, 1) && m_sio1_tx_empty)
			m_sio1_thre_irq = true;
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
		m_touch_irq_pending = false;
		pcmcia_update_irq();
	}

	if (m_touch_packet_pos < m_touch_packet_size)
		return m_touch_packet[m_touch_packet_pos++];
	return 0xff;
}

void ibmsimon_state::touch_data_w(u8 data)
{
	// Port 0170h is the data latch on Simon's external board interface as well
	// as the digitizer data port.  The production diagnostic toggles bit 7 as
	// BBh/3Bh, three times at roughly 0.58-second intervals; the schematic
	// polarity is active low.  Preserve every write so the green system LED,
	// boot sequencing and Run Diagnostics all follow the guest firmware.
	m_board_data_latch = data;

	// Bit 6 is also pulsed by the BIOS when it resets/calibrates the digitizer.
	// Coordinate reports are generated from the emulated pen inputs below.
	// Do not infer call answer from this latch: PHONE.EXE also selects audio
	// route 0 while leaving the phone screen.
	if (m_phone_ring)
		logerror("Simon audio route write %02X while ringing\n", data);
}

u8 ibmsimon_state::external_buttons_r()
{
	// Production diagnostics read the side keys from 0171h, then shift bits
	// 5:4 down and invert them.  The service manual calls this the I/O 172
	// latch, but the shipped diagnostic executable uses the odd byte address.
	// Both switches are normally high and pull their input low when pressed.
	u8 result = 0xff;
	if (BIT(m_external_buttons->read(), 1))
		result &= ~u8(0x20); // Up
	if (BIT(m_external_buttons->read(), 2))
		result &= ~u8(0x10); // Down
	return result;
}

u8 ibmsimon_state::touch_control_r()
{
	// The two volume buttons are wired directly to bits 5 and 4 of the Simon
	// board latch.  Both inputs are active low; the remaining bits retain the
	// output latch values written by the firmware.
	u8 result = m_touch_control | 0x30;
	if (BIT(m_external_buttons->read(), 1))
		result &= ~u8(0x20); // Volume Up
	if (BIT(m_external_buttons->read(), 2))
		result &= ~u8(0x10); // Volume Down
	return result;
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
		// Every RF power/reset test requires a fresh 21h POST response.  This is
		// separate from the host-switch registration handshake below: suppressing
		// repeated N packets must never suppress the hardware acknowledgement.
		sio1_rx_byte(0x21);
		m_rf_control_mode = true;
		if (!m_rf_ready_announced)
		{
			// Tell a connected virtual switch that the RF deck can now consume its
			// registration profile.  The switch may have accepted the socket before
			// BIOS POST, so its initial S line is not sufficient as a handshake.
			m_rf_ready_announced = true;
			cellular_link_text("N\n");
		}
		// Do not emit a complete unsolicited registration sequence on every
		// board-latch reset pulse.  PHONE.EXE pulses this line during its ordinary
		// audits; repeatedly injecting 83/84/Ax/64 creates needless COM1 IRQ load
		// and makes all Navigator applications sluggish.  The 7Fh phone-power
		// command and switch S profile already provide the required report once.
	}
}

INPUT_CHANGED_MEMBER(ibmsimon_state::power_button)
{
	if (!newval || oldval)
		return;

	if (!m_main_power)
	{
		pmu_resume(1); // EXT switch, never a reset or forced application exit
	}
	else if (!BIT(m_vg230_regs[0xc4], 1) && !BIT(m_vg230_regs[0x19], 0))
	{
		// EXT raises PMU NMI cause 001.  BIOS may defer it while DOS is busy;
		// never freeze the CPU in the middle of a card write or RF operation.
		m_vg230_regs[0x19] |= 0x01;
		m_vg230_regs[0xc0] = (m_vg230_regs[0xc0] & 0xe3) | 0x04;
		pcmcia_update_irq();
		logerror("Simon PMU EXT suspend request pc=%05X\n", m_maincpu->pc());
	}
}

void ibmsimon_state::pmu_resume(u8 source)
{
	if (m_main_power)
		return;
	// RTC and RF have independent power.  BIOS consumes the wake source and
	// restores its own PIT clock from RTC before returning to the application.
	m_vg230_regs[0xc0] = 0x80 | ((source & 3) << 5);
	m_vg230_regs[0xc1] |= 0x01;
	m_vg230_regs[0xda] &= ~u8(0x02);
	m_main_power = true;
	m_maincpu->resume(SUSPEND_REASON_DISABLE);
	logerror("Simon PMU resume source=%u RTC=%02u:%02u:%02u phone=%u\n", source,
		m_vg230_regs[0x72], m_vg230_regs[0x71], m_vg230_regs[0x70], m_phone_power);
}

void ibmsimon_state::pmu_ring()
{
	const u8 required = (m_vg230_regs[0xc2] >> 4) & 7;
	if (!m_main_power && required && ++m_phone_ring_pulses >= required)
		pmu_resume(3); // original BIOS has a dedicated RI-wake path at F8BF0
}

INPUT_CHANGED_MEMBER(ibmsimon_state::orientation_button)
{
	if (!newval || oldval)
		return;

	m_landscape = !m_landscape;
	if (render_target *const target = machine().render().first_target())
		target->set_view(m_landscape ? 1 : 0);
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::touch_tick)
{
	if (!m_main_power)
		return; // the unpowered pen is not a wake source for true SUSPEND
	// Each view has its own absolute input pair.  Re-scaling the portrait pair
	// after rotation made landscape X about 3.2 times too fast and introduced a
	// different Y gain.  Native-range landscape ports retain full edge reach
	// while using the same 16/50 long/short-axis calibration as portrait.
	const u16 x = m_landscape
		? std::min<u16>(m_pen_landscape_x->read(), 639)
		: std::min<u16>(m_pen_x->read(), 639);
	const u16 y = m_landscape
		? std::min<u16>(m_pen_landscape_y->read(), 199)
		: std::min<u16>(m_pen_y->read(), 199);

	// Keep the visible Simon stylus cursor and the digitizer packet on exactly
	// the same transformed coordinates.  This also avoids static crosshair-axis
	// definitions becoming wrong after a live orientation change.
	machine().crosshair().get_crosshair(0).setxy(
		float(x) / 639.0f,
		float(y) / 199.0f);
	m_stylus_x = x;
	m_stylus_y = y;

	// The BIOS consumes one byte per IRQ6.  Do not raise the next interrupt
	// until the previous byte has been read from port 0170h.
	if (m_touch_irq_pending)
		return;

	if (m_touch_packet_pos >= m_touch_packet_size)
	{
		const bool down = BIT(m_pen_button->read(), 0);

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

	m_touch_irq_pending = true;
	pcmcia_update_irq();
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::voice_tick)
{
	if (m_pager_beep_samples)
	{
		const double phase = 6.28318530717958647692 * 1050.0 * double(m_pager_beep_phase++) / 8000.0;
		m_voice_dac->data_w(u8(std::clamp(128 + int(52.0 * std::sin(phase)), 0, 255)));
		--m_pager_beep_samples;
	}
	else if (m_phone_call || m_network_busy_audio || m_call_request_sent)
	{
		// Buffer three 20 ms network intervals before starting the earpiece.
		// TCP arrivals and the emulation scheduler have different clocks; playing
		// the very first sample immediately inserts silence between every burst.
		if (!m_voice_rx_started && m_voice_rx_count >= 480)
			m_voice_rx_started = true;
		if (m_voice_rx_started && m_voice_rx_count)
		{
			const int sample = 128 + (int(m_voice_rx[m_voice_rx_head]) - 128) * m_phone_volume / 3;
			m_voice_dac->data_w(u8(std::clamp(sample, 0, 255)));
			m_voice_rx_head = (m_voice_rx_head + 1) % m_voice_rx.size();
			--m_voice_rx_count;
		}
		else
		{
			m_voice_rx_started = false;
			m_voice_dac->data_w(0x80);
		}
	}
	else
	{
		m_voice_rx_started = false;
		m_voice_dac->data_w(0x80);
	}

	if (!m_phone_call)
	{
		m_voice_tx_count = 0;
		m_dtmf_digit = 0;
		m_dtmf_samples = 0;
		return;
	}

	double sample = (!m_phone_muted && m_voice_input->exists())
		? std::clamp(m_voice_input->input(), -1.0, 1.0)
		: 0.0;

	if (m_dtmf_samples && m_dtmf_digit)
	{
		unsigned low = 0;
		unsigned high = 0;
		switch (m_dtmf_digit)
		{
		case '1': low = 697; high = 1209; break;
		case '2': low = 697; high = 1336; break;
		case '3': low = 697; high = 1477; break;
		case '4': low = 770; high = 1209; break;
		case '5': low = 770; high = 1336; break;
		case '6': low = 770; high = 1477; break;
		case '7': low = 852; high = 1209; break;
		case '8': low = 852; high = 1336; break;
		case '9': low = 852; high = 1477; break;
		case '*': low = 941; high = 1209; break;
		case '0': low = 941; high = 1336; break;
		case '#': low = 941; high = 1477; break;
		default: break;
		}
		if (low && high)
		{
			constexpr double phase_scale = 6.28318530717958647692 / 4294967296.0;
			const double tone = 0.32 * std::sin(double(m_dtmf_low_phase) * phase_scale)
				+ 0.32 * std::sin(double(m_dtmf_high_phase) * phase_scale);
			sample = std::clamp(sample * 0.35 + tone, -1.0, 1.0);
			m_dtmf_low_phase += u32((u64(low) << 32) / 8000);
			m_dtmf_high_phase += u32((u64(high) << 32) / 8000);
		}
		if (!--m_dtmf_samples)
			m_dtmf_digit = 0;
	}

	m_voice_tx[m_voice_tx_count++] = u8(std::clamp<int>(int(sample * 127.0) + 128, 0, 255));
	if (m_voice_tx_count == m_voice_tx.size())
	{
		static constexpr char hex[] = "0123456789ABCDEF";
		std::array<char, 2 + 160 * 2 + 1> frame;
		frame[0] = 'Y';
		frame[1] = ' ';
		unsigned pos = 2;
		for (u8 value : m_voice_tx)
		{
			frame[pos++] = hex[value >> 4];
			frame[pos++] = hex[value & 0x0f];
		}
		frame[pos] = '\n';
		cellular_link_text(std::string_view(frame.data(), frame.size()));
		m_voice_tx_count = 0;
	}
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::cellular_link_tick)
{
	if (!m_cellular_link->exists())
		return;
	// Drain available bursts independently of the 20 Hz RF state sequencer.
	// A single read can return only part of a TCP burst; limiting this to one
	// read every 50 ms allowed voice packets to delay R/C/H control messages.
	u8 input[4096];
	for (unsigned burst = 0; burst != 8; ++burst)
	{
		// Use a fixed-size read_some rather than image fread's fill-buffer loop.
		// Its final short read enables core_file read-ahead, which a following
		// write invalidates.  On a duplex socket those unread bytes are lost.
		std::size_t received = 0;
		m_cellular_link->image_core_file().read_some(input, sizeof(input), received);
		if (!received)
			break;
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
	}
}

TIMER_CALLBACK_MEMBER(ibmsimon_state::cellular_tick)
{
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
			// A7 was latched before this state.  Give PHONE.EXE time to ring and
			// offer Answer/End; an enabled deck then performs unattended answer.
			rf_schedule_event(12, 120); // six seconds at the 20 Hz control rate
			break;
		case 3:
			sio1_rx_byte(0x65); // originating
			m_rf_event_phase = 0;
			break;
		case 4:
			m_phone_call = true;
			m_phone_muted = false;
			m_network_busy_audio = false;
			if (m_phone_auto_answering)
			{
				// Pager bearer is private to the RF deck.  PHONE.EXE must remain on
				// its idle screen while DTMF is collected in the background.
				m_rf_event_phase = 0;
				break;
			}
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
			if (m_phone_ring)
				pmu_ring();
			// A7 is a persistent incoming-call latch, not the audible ring
			// waveform.  It was already asserted in cellular_start_ringing();
			// PHONE.EXE generates the audible cadence and runs the configured
			// unattended-answer timer while that latch remains set.  In particular,
			// A8 must not be sent between rings: BIOS reports A8 as a released,
			// unanswered call and PHONE.EXE consequently opens the missed-call box.
			++m_phone_ring_pulses;
			if (m_phone_ring && m_phone_accept_pages && m_phone_auto_answer && !m_answer_request_sent)
			{
				m_phone_auto_answering = true;
				m_answer_request_sent = true;
				m_rf_event_phase = 0;
				cellular_link_text("A\n");
			}
			else
			{
				m_rf_event_phase = 0;
			}
			break;
		case 13:
			m_rf_event_phase = 0; // retained for save-state compatibility
			break;
		}
	}

	// In real AMPS the mobile continuously returns the assigned SAT and the
	// base checks it at least every 250 ms.  SAT (about 6 kHz) and the 10 kHz
	// signalling tone sit above this emulator's 8 kHz audible PCM Nyquist
	// limit, so the RF deck reports its supervisory state separately.  Speech
	// and DTMF remain genuine samples on the Y voice bearer.
	if (m_phone_call)
	{
		if (++m_sat_report_ticks >= 5)
		{
			m_sat_report_ticks = 0;
			cellular_link_text("Z ");
			cellular_link_text(std::to_string(m_cellular_sat));
			cellular_link_text("\n");
		}
	}
	else
	{
		m_sat_report_ticks = 0;
	}

	const u8 system = m_cellular_system->read() & 0x07;
	if (m_cellular_registration == 0xff && m_rf_control_mode && m_phone_power && system != m_last_cellular_system)
	{
		sio1_rx_byte(0x84 + system);
		m_last_cellular_system = system;
	}

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
	map(0x0062, 0x0062).lrw8(NAME([this]() { return ppi_c_r(); }), NAME([](u8) { }));
	map(0x006c, 0x006c).lrw8(NAME([this]() { return m_map_address; }), NAME([this](u8 data) { m_map_address = data; }));
	map(0x006e, 0x006f).rw(FUNC(ibmsimon_state::map_data_r), FUNC(ibmsimon_state::map_data_w));
	map(0x00a0, 0x00a0).w(m_mb, FUNC(pc_noppi_mb_device::nmi_enable_w));
	map(0x0080, 0x0083).lrw8(NAME([this](offs_t offset) { return m_dma_page_regs[offset]; }), NAME([this](offs_t offset, u8 data) { m_dma_page_regs[offset] = data; }));
	map(0x0170, 0x0170).rw(FUNC(ibmsimon_state::touch_data_r), FUNC(ibmsimon_state::touch_data_w));
	map(0x0171, 0x0171).r(FUNC(ibmsimon_state::external_buttons_r));
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
	// The green system LED is bit 7 of the external board-data latch at 0170h,
	// active low.  Main suspend removes its supply independently of the latch.
	m_power_led = m_main_power && !BIT(m_board_data_latch, 7);
	m_phone_led = m_phone_power;
	m_backlight_led = m_main_power && m_lcd_backlight;
	auto &pointer = machine().crosshair().get_crosshair(0);
	pointer.set_mode(m_main_power ? CROSSHAIR_VISIBILITY_ON : CROSSHAIR_VISIBILITY_OFF);
	pointer.set_visible(m_main_power);
	if (!m_main_power)
	{
		// Unlike a backlight timeout, SUSPEND removes LCD bias and blanks it.
		bitmap.fill(rgb_t(0xa9, 0xad, 0x91), cliprect);
		return 0;
	}

	// Simon's transflective LCD remains readable without its electroluminescent
	// backlight.  PWRON VP0 enables the panel supply while CFh's activity timer
	// controls the visible backlight state emulated above.
	const bool backlight = m_lcd_backlight;
	const rgb_t paper = backlight ? rgb_t(0xf4, 0xf2, 0xdf) : rgb_t(0xa9, 0xad, 0x91);
	const rgb_t ink = backlight ? rgb_t(0x18, 0x1b, 0x18) : rgb_t(0x43, 0x48, 0x3c);
	// VG230 CRTC index C2h bit 0 reverses the LCD polarity.  Simon sets it
	// during Navigator startup, after drawing the dark-on-light boot hourglass.
	const bool reverse_video = BIT(m_crtc_regs[0xc2], 0);
	const rgb_t pixel_on = reverse_video ? paper : ink;
	const rgb_t pixel_off = reverse_video ? ink : paper;
	bitmap.fill(paper, cliprect);

	// The Navigator normally leaves the VG230-compatible CGA block in
	// 640x200 APA mode.  ROM-DOS still uses the same LCD as an ordinary
	// 80-column text console after INT 10h selects mode 3.  The original
	// implementation always interpreted B8000 as the APA bitmap, which made a
	// genuine COMMAND.COM prompt look like vertical stripes (or left the last
	// Navigator frame visible when the BIOS console was not initialised).
	if (!BIT(m_cga_mode, 1))
	{
		// Award's Simon BIOS contains the standard 8x8 PC font at F000:FA6E.
		const u8 *const font = reinterpret_cast<const u8 *>(&m_bios[0]) + 0xfa6e;
		const unsigned start = ((((unsigned(m_crtc_regs[0x0c]) & 0x3f) << 8) |
			m_crtc_regs[0x0d]) * 2) & 0x7fff;
		const unsigned cursor = ((unsigned(m_crtc_regs[0x0e]) << 8) |
			m_crtc_regs[0x0f]) & 0x3fff;
		const bool cursor_visible = !BIT(m_crtc_regs[0x0a], 5) &&
			BIT(machine().time().as_ticks(2), 0);
		const unsigned cursor_first = m_crtc_regs[0x0a] & 0x1f;
		const unsigned cursor_last = m_crtc_regs[0x0b] & 0x1f;

		for (int y = cliprect.min_y; y <= cliprect.max_y && y < 200; ++y)
		{
			const unsigned row = unsigned(y) >> 3;
			const unsigned glyph_y = unsigned(y) & 7;
			for (int x = cliprect.min_x; x <= cliprect.max_x && x < 640; ++x)
			{
				const unsigned column = unsigned(x) >> 3;
				const unsigned cell = row * 80 + column;
				const unsigned address = (start + cell * 2) & 0x7fff;
				const u8 character = m_video_ram[address];
				bool pixel = BIT(font[unsigned(character) * 8 + glyph_y], 7 - (unsigned(x) & 7));
				if (cursor_visible && cell == cursor && glyph_y >= cursor_first && glyph_y <= cursor_last)
					pixel = true;
				bitmap.pix(y, x) = pixel ? pixel_on : pixel_off;
			}
		}
		return 0;
	}

	// VG230 mode 640x200 APA uses the PC-compatible CGA odd/even 8 KiB
	// scan-line banks.  Simon firmware selects this mode for its GUI.
	const unsigned start = (((unsigned(m_crtc_regs[0x0c]) & 0x3f) << 8) | m_crtc_regs[0x0d]) * 2;
	for (int y = cliprect.min_y; y <= cliprect.max_y && y < 200; ++y)
	{
		for (int x = cliprect.min_x; x <= cliprect.max_x && x < 640; ++x)
		{
			const unsigned address = start + ((y & 1) ? 0x2000 : 0) + (y >> 1) * 80 + (x >> 3);
			bitmap.pix(y, x) = BIT(m_video_ram[address & 0x7fff], 7 - (x & 7)) ? pixel_on : pixel_off;
		}
	}
	return 0;
}

static INPUT_PORTS_START(ibmsimon)
	PORT_START("PENX")
	// ROT90 displays native (x,y) at (199-y,x): host Y therefore drives
	// native X, while host X drives the reversed native Y axis.  The unequal
	// numeric ranges require the original 16/50 calibration for matched motion.
	PORT_BIT(0x03ff, 0x013f, IPT_LIGHTGUN_Y) PORT_NAME("Pen Native X / Window Y") PORT_MINMAX(0, 639) PORT_SENSITIVITY(16) PORT_KEYDELTA(2) PORT_CROSSHAIR(X, 1.0, 0.0, 0)

	PORT_START("PENY")
	PORT_BIT(0x03ff, 0x0063, IPT_LIGHTGUN_X) PORT_NAME("Pen Native Y / Window X") PORT_MINMAX(0, 199) PORT_SENSITIVITY(50) PORT_KEYDELTA(3) PORT_REVERSE PORT_CROSSHAIR(Y, 1.0, 0.0, 0)

	PORT_START("PENLX")
	PORT_BIT(0x03ff, 0x013f, IPT_LIGHTGUN_X) PORT_NAME("Landscape Pen X") PORT_MINMAX(0, 639) PORT_SENSITIVITY(16) PORT_KEYDELTA(2)

	PORT_START("PENLY")
	PORT_BIT(0x03ff, 0x0063, IPT_LIGHTGUN_Y) PORT_NAME("Landscape Pen Y") PORT_MINMAX(0, 199) PORT_SENSITIVITY(50) PORT_KEYDELTA(3)

	PORT_START("PEN")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_BUTTON1) PORT_NAME("Touch Screen / Stylus") PORT_CODE(MOUSECODE_BUTTON1)

	PORT_START("BUTTONS")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_OTHER) PORT_NAME("Simon Power / Resume-Suspend") PORT_CODE(KEYCODE_F10) PORT_CHANGED_MEMBER(DEVICE_SELF, FUNC(ibmsimon_state::power_button), 0)
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_OTHER) PORT_NAME("Simon Volume Up") PORT_CODE(KEYCODE_F8) PORT_CODE(KEYCODE_PGUP)
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_OTHER) PORT_NAME("Simon Volume Down") PORT_CODE(KEYCODE_F7) PORT_CODE(KEYCODE_PGDN)
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_OTHER) PORT_NAME("Rotate Portrait / Landscape") PORT_CODE(KEYCODE_F9) PORT_CHANGED_MEMBER(DEVICE_SELF, FUNC(ibmsimon_state::orientation_button), 0)

	PORT_START("CELLULAR_SYSTEM")
	PORT_CONFNAME(0x07, 0x00, "Virtual 1G Registration")
	PORT_CONFSETTING(0x00, "Home 1")
	PORT_CONFSETTING(0x01, "Home 2")
	PORT_CONFSETTING(0x02, "Home 3")
	PORT_CONFSETTING(0x03, "Home 4")
	PORT_CONFSETTING(0x04, "Roam")
	PORT_CONFSETTING(0x05, "Alternate Roam")

INPUT_PORTS_END

static void simon_pcmcia_devices(device_slot_interface &device)
{
	// Simon's software cards are battery-backed Type II SRAM cards.  The
	// Mitsubishi device has no CIS attribute ROM, matching early DOS memory
	// cards formatted directly as block media by Card Services/PCDISK.
	device.option_add("melcard_1m", PCCARD_SRAM_MITSUBISHI_1M);
	device.option_add("melcard_1800k", PCCARD_SRAM_MITSUBISHI_1800K);
	device.option_add("melcard_4m", PCCARD_SRAM_MITSUBISHI_4M);
	device.option_add("linear16", FUJITSU_16MB_FLASH_CARD);
	device.option_add("ata", ATA_FLASH_PCCARD);
}

void ibmsimon_state::ibmsimon(machine_config &config)
{
	V30(config, m_maincpu, 16_MHz_XTAL);
	m_maincpu->set_addrmap(AS_PROGRAM, &ibmsimon_state::mem_map);
	m_maincpu->set_addrmap(AS_IO, &ibmsimon_state::io_map);
	m_maincpu->set_irq_acknowledge_callback(FUNC(ibmsimon_state::irq_acknowledge));
	GENERIC_KEYBOARD(config, "keyboard").set_keyboard_callback(FUNC(ibmsimon_state::dos_keyboard_put));

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
	// The VG230 derives the PC-compatible 8254 timer clock at 1.19318 MHz.
	// Channel 2 drives the system speaker and must use the same input clock as
	// channels 0 and 1; halving only this channel lowers pitch and cadence by 2x.
	pit.set_clk<2>(XTAL(14'318'181) / 12.0);
	pit.out_handler<2>().set(m_mb, FUNC(pc_noppi_mb_device::pc_pit8253_out2_changed));

	// The VG230 data manual specifies a low-pass filter between SPKR and the
	// physical transducer.  Without it, the ideal one-bit square wave sounds
	// unnaturally harsh and its high harmonics resemble a sped-up chirp.
	filter_rc_device &speaker_filter(FILTER_RC(config, "speaker_filter"));
	speaker_filter.set_lowpass(RES_K(10), CAP_N(10)).add_route(ALL_OUTPUTS, "mb:mono", 1.0);
	subdevice<speaker_sound_device>("mb:speaker")->reset_routes().add_route(ALL_OUTPUTS, ":speaker_filter", 1.0);

	// The AMPS voice path is analogue and never enters Simon's V30 address
	// space.  A WAV mounted as -voicein supplies the handset microphone; peer
	// PCM received through the virtual switch is rendered by an 8-bit DAC.
	CASSETTE(config, m_voice_input).set_default_state(CASSETTE_PLAY | CASSETTE_MOTOR_ENABLED | CASSETTE_SPEAKER_MUTED);
	DAC_8BIT_R2R(config, m_voice_dac, 0).add_route(ALL_OUTPUTS, "mb:mono", 0.45);

	RAM(config, m_mainram).set_default_size("512K");

	// A real Simon normally boots with an empty PC Card slot.  Making an SRAM
	// card the slot default causes Card Services to probe it during CONFIG.SYS;
	// until the emulated card is fully compatible that can leave the guest in a
	// permanent hourglass before the UI becomes usable.  Cards remain available
	// explicitly with -pcmcia melcard_1m (or -pcmcia ata).
	PCCARD_SLOT(config, m_pcmcia, simon_pcmcia_devices, nullptr);
	m_pcmcia->cd1().set(FUNC(ibmsimon_state::pcmcia_cd1_w));
	m_pcmcia->cd2().set(FUNC(ibmsimon_state::pcmcia_cd2_w));
	m_pcmcia->bvd1().set(FUNC(ibmsimon_state::pcmcia_bvd1_w));
	m_pcmcia->bvd2().set(FUNC(ibmsimon_state::pcmcia_bvd2_w));
	m_pcmcia->wp().set(FUNC(ibmsimon_state::pcmcia_wp_w));

	screen_device &screen(SCREEN(config, "screen").set_lcd());
	screen.set_refresh_hz(70);
	screen.set_vblank_time(ATTOSECONDS_IN_USEC(2500));
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
COMP(1994, ibmsimon, 0,      0,      ibmsimon, ibmsimon, ibmsimon_state, empty_init, "IBM",   "Simon Personal Communicator",       MACHINE_SUPPORTS_SAVE)
