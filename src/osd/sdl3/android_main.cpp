#ifdef __ANDROID__

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <jni.h>
#include <mutex>
#include <thread>
#include <SDL3/SDL_audio.h>

extern "C" int SDL_main(int argc, char *argv[]);

// Android delivers battery broadcasts on the UI thread. Simon's PMU reads
// the last reported low-battery state on the emulation thread.
static std::atomic_bool simon_battery_low{false};

extern "C" JNIEXPORT void JNICALL
Java_org_interinit_simonulator_SimonActivity_setHostBatteryLow(JNIEnv *, jclass, jboolean low)
{
	simon_battery_low.store(low == JNI_TRUE, std::memory_order_relaxed);
}

extern "C" bool simon_android_battery_low()
{
	return simon_battery_low.load(std::memory_order_relaxed);
}

namespace {

// The switch exchanges 20 ms frames. Keep at most 80 ms of Android audio so
// a delayed emulation frame cannot make the listener hear old conversation.
constexpr size_t packet_samples = 160;
using packet = std::array<int16_t, packet_samples>;

class handset_microphone
{
public:
	void permit(bool permitted)
	{
		std::lock_guard lock(m_mutex);
		m_permitted = permitted;
		if (!permitted) m_samples.clear();
		m_changed.notify_one();
	}

	int16_t sample(bool requested)
	{
		// The emulator is the only caller of sample(). The Android UI thread
		// updates permission and the recording thread supplies whole packets.
		if (requested != m_last_request) {
			std::lock_guard lock(m_mutex);
			m_requested = m_last_request = requested;
			m_position = packet_samples;
			m_samples.clear();
			if (requested && !m_thread.joinable())
				m_thread = std::thread(&handset_microphone::record, this);
			m_changed.notify_one();
		}
		if (!requested) return 0;
		if (m_position == packet_samples) {
			std::lock_guard lock(m_mutex);
			const auto count = std::min(m_samples.size(), packet_samples);
			std::copy_n(m_samples.begin(), count, m_packet.begin());
			m_samples.erase(m_samples.begin(), m_samples.begin() + count);
			std::fill(m_packet.begin() + count, m_packet.end(), 0);
			m_position = 0;
		}
		return m_packet[m_position++];
	}

	void stop()
	{
		{
			std::lock_guard lock(m_mutex);
			m_stopping = true;
			m_changed.notify_one();
		}
		if (m_thread.joinable()) m_thread.join();
		std::lock_guard lock(m_mutex);
		m_samples.clear();
		m_requested = m_last_request = m_stopping = false;
		m_position = packet_samples;
	}

private:
	void record()
	{
		using namespace std::chrono_literals;
		const SDL_AudioSpec format{SDL_AUDIO_S16, 1, 8000};
		packet captured;
		std::unique_lock lock(m_mutex);
		auto inactive = [this] { return m_stopping || !m_requested || !m_permitted; };
		while (!m_stopping) {
			m_changed.wait(lock, [this] { return m_stopping || (m_requested && m_permitted); });
			if (m_stopping) break;
			lock.unlock();
			// SDL converts the device's native rate and format to the bearer
			// format. Android can temporarily refuse an occupied microphone.
			SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &format, nullptr, nullptr);
			if (stream && !SDL_ResumeAudioStreamDevice(stream)) {
				SDL_DestroyAudioStream(stream);
				stream = nullptr;
			}
			lock.lock();
			if (!stream) {
				m_changed.wait_for(lock, 1s, inactive);
				continue;
			}
			int bytes = 0;
			while (!inactive()) {
				lock.unlock();
				bytes = SDL_GetAudioStreamData(stream, captured.data(), sizeof(captured));
				lock.lock();
				if (bytes < 0 || inactive()) break;
				m_samples.insert(m_samples.end(), captured.begin(), captured.begin() + bytes / sizeof(int16_t));
				if (m_samples.size() > 4 * packet_samples)
					m_samples.erase(m_samples.begin(), m_samples.end() - 4 * packet_samples);
				if (bytes < int(sizeof(captured))) m_changed.wait_for(lock, 10ms, inactive);
			}
			lock.unlock();
			SDL_DestroyAudioStream(stream);
			lock.lock();
			if (bytes < 0) m_changed.wait_for(lock, 1s, inactive);
		}
	}

	std::mutex m_mutex;
	std::condition_variable m_changed;
	std::thread m_thread;
	std::deque<int16_t> m_samples;
	bool m_permitted = false, m_requested = false, m_stopping = false;
	packet m_packet{};
	size_t m_position = packet_samples;
	bool m_last_request = false;
};

handset_microphone microphone;
}

extern "C" JNIEXPORT void JNICALL
Java_org_interinit_simonulator_SimonActivity_setHostRecordingAllowed(JNIEnv *, jclass, jboolean allowed)
{
	microphone.permit(allowed == JNI_TRUE);
}

extern "C" int16_t simon_android_microphone_sample(bool active) { return microphone.sample(active); }
extern "C" void simon_android_close_microphone() { microphone.stop(); }

// Using this in main library to prevent linker removing SDL_main
int dummy_main(int argc, char** argv)
{
	return SDL_main(argc, argv);
}

#endif /* __ANDROID__ */
