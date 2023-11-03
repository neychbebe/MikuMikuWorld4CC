#include "../Application.h"
#include "../IO.h"
#include "../UI.h"

// We need add the implementation defines BEFORE including miniaudio's header
#define MINIAUDIO_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include "AudioManager.h"
#include <execution>

#undef STB_VORBIS_HEADER_ONLY

namespace Audio
{
	namespace mmw = MikuMikuWorld;

	void AudioManager::initializeAudioEngine()
	{
		std::string err = "";
		ma_result result = MA_SUCCESS;

		try
		{
			result = ma_engine_init(nullptr, &engine);
			if (result != MA_SUCCESS)
			{
				err = "FATAL: Failed to start audio engine. Aborting.\n";
				throw(result);
			}

			result = ma_sound_group_init(&engine, maSoundFlagsDefault, nullptr, &musicGroup);
			if (result != MA_SUCCESS)
			{
				err = "FATAL: Failed to initialize music sound group. Aborting.\n";
				throw(result);
			}

			result = ma_sound_group_init(&engine, maSoundFlagsDefault, nullptr, &soundEffectsGroup);
			if (result != MA_SUCCESS)
			{
				err = "FATAL: Failed to initialize sound effects sound group. Aborting.\n";
				throw(result);
			}
		}
		catch (ma_result)
		{
			err.append(ma_result_description(result));
			IO::messageBox(APP_NAME, err, IO::MessageBoxButtons::Ok, IO::MessageBoxIcon::Error);

			exit(result);
		}

		setMasterVolume(1.0f);
		setMusicVolume(1.0f);
		setSoundEffectsVolume(1.0f);
	}

	void AudioManager::loadSoundEffects()
	{
		constexpr int soundEffectsCount = sizeof(mmw::SE_NAMES) / sizeof(const char*);
		constexpr std::array<SoundFlags, soundEffectsCount> soundEffectsFlags =
		{
			NONE, NONE, NONE, NONE, LOOP | EXTENDABLE, NONE, NONE, NONE, NONE, LOOP | EXTENDABLE
		};

		constexpr std::array<float, soundEffectsCount> soundEffectsVolumes =
		{
			0.75f, 0.75f, 0.90f, 0.80f, 0.70f, 0.75f, 0.80f, 0.92f, 0.82f, 0.70f
		};

		std::string path{ mmw::Application::getAppDir() + "res\\sound\\" };

		sounds.reserve(soundEffectsCount);
		debugSounds.resize(soundEffectsCount);
		for (int i = 0; i < soundEffectsCount; ++i)
			sounds.emplace(std::move(SoundPoolPair(mmw::SE_NAMES[i], std::make_unique<SoundPool>())));

		std::for_each(std::execution::par, sounds.begin(), sounds.end(), [&](auto& s)
		{
			std::string filename = path + s.first.data() + ".mp3";
			int soundIndex = mmw::findArrayItem(s.first.data(), mmw::SE_NAMES, mmw::arrayLength(mmw::SE_NAMES));

			s.second->initialize(filename, &engine, &soundEffectsGroup, soundEffectsFlags[soundIndex]);
			s.second->setVolume(soundEffectsVolumes[soundIndex]);

			ma_sound_init_from_file_w(&engine, IO::mbToWideStr(filename).c_str(), maSoundFlagsDecodeAsync, &soundEffectsGroup, nullptr, &debugSounds[soundIndex].source);
		});
		
		// Adjust hold SE loop times for gapless playback
		ma_uint64 holdNrmDuration = sounds[mmw::SE_CONNECT]->getDurationInFrames();
		ma_uint64 holdCrtDuration = sounds[mmw::SE_CRITICAL_CONNECT]->getDurationInFrames();
		sounds[mmw::SE_CONNECT]->setLoopTime(3000, holdNrmDuration - 3000);
		sounds[mmw::SE_CRITICAL_CONNECT]->setLoopTime(3000, holdCrtDuration - 3000);
	}

	void AudioManager::uninitializeAudioEngine()
	{
		disposeMusic();
		for (auto& [name, sound] : sounds)
			sound->dispose();

		sounds.clear();

		ma_engine_uninit(&engine);
	}

	mmw::Result AudioManager::loadMusic(const std::string& filename)
	{
		disposeMusic();
		mmw::Result result = decodeAudioFile(filename, musicBuffer);
		if (result.isOk())
		{
			ma_sound_init_from_data_source(&engine, &musicBuffer.buffer, maSoundFlagsDefault, &musicGroup, &music);
		}

		return result;
	}

	void AudioManager::playMusic(float currentTime)
	{
		ma_uint64 length{};
		ma_sound_get_length_in_pcm_frames(&music, &length);

		// Negative time means the sound is midways
		float time = musicOffset - currentTime;

		// Starting past the music end
		if (time * musicBuffer.sampleRate * -1 > length)
			return;

		ma_sound_set_start_time_in_milliseconds(&music, std::max(0.0f, time * 1000));
		ma_sound_start(&music);
	}

	void AudioManager::stopMusic()
	{
		ma_sound_stop(&music);
	}

	void AudioManager::setMusicOffset(float currentTime, float offset)
	{
		musicOffset = offset / 1000.0f;
		float seekTime = currentTime - musicOffset;
		ma_sound_seek_to_pcm_frame(&music, seekTime * musicBuffer.sampleRate);

		float start = getAudioEngineAbsoluteTime() + musicOffset - currentTime;
		ma_sound_set_start_time_in_milliseconds(&music, std::max(0.0f, start * 1000));
	}

	float AudioManager::getMusicPosition()
	{
		float cursor{};
		ma_sound_get_cursor_in_seconds(&music, &cursor);

		return cursor;
	}

	float AudioManager::getMusicLength()
	{
		float length{};
		ma_sound_get_length_in_seconds(&music, &length);

		return length;
	}

	void AudioManager::disposeMusic()
	{
		if (musicBuffer.isValid())
		{
			ma_sound_stop(&music);
			ma_sound_uninit(&music);
			musicBuffer.dispose();
		}
	}

	void AudioManager::seekMusic(float time)
	{
		ma_uint64 seekFrame = (time - musicOffset) * musicBuffer.sampleRate;
		ma_sound_seek_to_pcm_frame(&music, seekFrame);

		ma_uint64 length{};
		ma_result lengthResult = ma_sound_get_length_in_pcm_frames(&music, &length);
		if (lengthResult != MA_SUCCESS)
			return;

		if (seekFrame > length)
		{
			// Seeking beyond the sound's length
			music.atEnd = true;
		}
		else if (ma_sound_at_end(&music) && seekFrame < length)
		{
			// Sound reached the end but sought to an earlier frame
			music.atEnd = false;
		}
	}

	float AudioManager::getMasterVolume() const
	{
		return masterVolume;
	}

	void AudioManager::setMasterVolume(float volume)
	{
		masterVolume = volume;
		ma_engine_set_volume(&engine, volume);
	}

	float AudioManager::getMusicVolume() const
	{
		return musicVolume;
	}

	void AudioManager::setMusicVolume(float volume)
	{
		musicVolume = volume;
		ma_sound_group_set_volume(&musicGroup, volume);
	}

	float AudioManager::getSoundEffectsVolume() const
	{
		return soundEffectsVolume;
	}

	void AudioManager::setSoundEffectsVolume(float volume)
	{
		soundEffectsVolume = volume;
		ma_sound_group_set_volume(&soundEffectsGroup, volume);
	}

	void AudioManager::playOneShotSound(std::string_view name)
	{
		if (sounds.find(name) == sounds.end())
			return;

		sounds.at(name)->play(0, -1);
	}

	void AudioManager::playSoundEffect(std::string_view name, float start, float end)
	{
		if (sounds.find(name) == sounds.end())
			return;

		sounds.at(name)->play(start, end);
	}

	void AudioManager::stopSoundEffects(bool all)
	{
		if (all)
		{
			for (auto& [se, sound] : sounds)
				sound->stopAll();
		}
		else
		{
			sounds[mmw::SE_CONNECT]->stopAll();
			sounds[mmw::SE_CRITICAL_CONNECT]->stopAll();

			// Also stop any scheduled sounds
			for (auto& [se, sound] : sounds)
			{
				for (auto& instance : sound->pool)
				{
					ma_uint64 cursor{};
					ma_sound_get_cursor_in_pcm_frames(&instance.source, &cursor);
					if (cursor <= 0)
						ma_sound_stop(&instance.source);
				}
			}
		}
	}

	uint32_t AudioManager::getDeviceChannelCount() const
	{
		return engine.pDevice->playback.channels;
	}

	float AudioManager::getDeviceLatency() const
	{
		return engine.pDevice->playback.internalPeriodSizeInFrames / static_cast<float>(engine.pDevice->playback.internalSampleRate);
	}

	uint32_t AudioManager::getDeviceSampleRate() const
	{
		return engine.pDevice->playback.internalSampleRate;
	}

	float AudioManager::getAudioEngineAbsoluteTime() const
	{
		// Engine time is in milliseconds
		return static_cast<float>(ma_engine_get_time(&engine)) / static_cast<float>(engine.sampleRate) / 1000.0f;
	}

	float AudioManager::getMusicOffset() const
	{
		return musicOffset;
	}

	float AudioManager::getMusicEndTime()
	{
		float length = 0.0f;
		ma_sound_get_length_in_seconds(&music, &length);

		return length + musicOffset;
	}

	void AudioManager::syncAudioEngineTimer()
	{
		ma_engine_set_time(&engine, 0);
	}

	bool AudioManager::isMusicInitialized() const
	{
		return musicBuffer.isValid();
	}

	bool AudioManager::isMusicAtEnd() const
	{
		return ma_sound_at_end(&music);
	}

	bool AudioManager::isSoundPlaying(std::string_view name) const
	{
		if (sounds.find(name) == sounds.end())
			return false;

		return sounds.at(name)->isAnyPlaying();
	}
}
