
// by xsh
// 2022/03/17
#include <string>
#include <iostream>
#include <unistd.h>
#include <iomanip>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include "api/echo_canceller3_factory.h"
#include "api/echo_canceller3_config.h"
#include "audio_processing/include/audio_processing.h"
#include "audio_processing/audio_buffer.h"
#include "audio_processing/high_pass_filter.h"

#include "wavreader.h"
#include "wavwriter.h"
#include "demo/print_tool.h"

using namespace webrtc;
using namespace std;

void print_wav_information(const char *fn, int format, int channels, int sample_rate, int bits_per_sample, int length)
{
	cout << "=====================================" << endl
		 << fn << " information:" << endl
		 << "format: " << format << endl
		 << "channels: " << channels << endl
		 << "sample_rate: " << sample_rate << endl
		 << "bits_per_sample: " << bits_per_sample << endl
		 << "length: " << length << endl
		 << "total_samples: " << length / bits_per_sample * 8 << endl
		 << "======================================" << endl;
}

int main(int argc, char *argv[])
{

	if (argc != 4)
	{
		cerr << "usage: ./demo ref.wav rec.wav out.wav" << endl;
		return -1;
	}
	cout << "======================================" << endl
		 << "ref file is: " << argv[1] << endl
		 << "rec file is: " << argv[2] << endl
		 << "out file is: " << argv[3] << endl
		 << "======================================" << endl;

	void *h_ref = wav_read_open(argv[1]);
	void *h_rec = wav_read_open(argv[2]);

	int ref_format, ref_channels, ref_sample_rate, ref_bits_per_sample;
	int rec_format, rec_channels, rec_sample_rate, rec_bits_per_sample;
	unsigned int ref_data_length, rec_data_length;

	int res = wav_get_header(h_ref, &ref_format, &ref_channels, &ref_sample_rate, &ref_bits_per_sample, &ref_data_length);
	if (!res)
	{
		cerr << "get ref header error: " << res << endl;
		return -1;
	}
	int ref_samples = ref_data_length * 8 / ref_bits_per_sample / ref_channels;
	print_wav_information(argv[1], ref_format, ref_channels, ref_sample_rate, ref_bits_per_sample, ref_data_length);

	res = wav_get_header(h_rec, &rec_format, &rec_channels, &rec_sample_rate, &rec_bits_per_sample, &rec_data_length);
	if (!res)
	{
		cerr << "get rec header error: " << res << endl;
		return -1;
	}
	int rec_samples = rec_data_length * 8 / rec_bits_per_sample / rec_channels;
	print_wav_information(argv[2], rec_format, rec_channels, rec_sample_rate, rec_bits_per_sample, rec_data_length);

	if (ref_format != rec_format ||
		ref_sample_rate != rec_sample_rate ||
		ref_bits_per_sample != rec_bits_per_sample)
	{
		cerr << "ref file format != rec file format" << endl;
		return -1;
	}

	EchoCanceller3Config aec_config;
	aec_config.filter.export_linear_aec_output = true;
	aec_config.delay.use_external_delay_estimator = true;

	EchoCanceller3Factory aec_factory = EchoCanceller3Factory(aec_config);
	std::unique_ptr<EchoControl> echo_controler = aec_factory.Create(ref_sample_rate, ref_channels, rec_channels);
	std::unique_ptr<HighPassFilter> hp_filter = std::make_unique<HighPassFilter>(rec_sample_rate, rec_channels);

	int sample_rate = rec_sample_rate;
	int channels = rec_channels;
	// The processing pipeline expects 16-bit signed integers.
	const int processing_bits_per_sample = 16;

	std::unique_ptr<AudioBuffer> ref_audio = std::make_unique<AudioBuffer>(
		sample_rate, ref_channels,
		sample_rate, ref_channels,
		sample_rate, ref_channels);
	std::unique_ptr<AudioBuffer> aec_audio = std::make_unique<AudioBuffer>(
		sample_rate, rec_channels,
		sample_rate, rec_channels,
		sample_rate, rec_channels);
	std::unique_ptr<AudioBuffer> aec_linear_audio = std::make_unique<AudioBuffer>(
		sample_rate, rec_channels,
		sample_rate, rec_channels,
		sample_rate, rec_channels);

	AudioFrame ref_frame, aec_frame;

	void *h_out = wav_write_open(argv[3], rec_sample_rate, processing_bits_per_sample, rec_channels);
	void *h_linear_out = wav_write_open("linear.wav", sample_rate, processing_bits_per_sample, rec_channels);

	int num_frames = sample_rate / 100;
	int bytes_per_frame_rec_in = num_frames * rec_channels * rec_bits_per_sample / 8;
	int bytes_per_frame_ref_in = num_frames * ref_channels * ref_bits_per_sample / 8;
	int bytes_per_frame_rec_out = num_frames * rec_channels * processing_bits_per_sample / 8;
	int total = rec_samples < ref_samples ? rec_samples / num_frames : rec_samples / num_frames;

	int current = 0;
	unsigned char *ref_tmp = new unsigned char[bytes_per_frame_ref_in];
	size_t aec_tmp_size = bytes_per_frame_rec_in > bytes_per_frame_rec_out ? bytes_per_frame_rec_in : bytes_per_frame_rec_out;
	unsigned char *aec_tmp = new unsigned char[aec_tmp_size];
	unsigned char *aec_out_tmp = new unsigned char[bytes_per_frame_rec_out];
	int16_t *ref_converted = new int16_t[num_frames * ref_channels];
	int16_t *aec_converted = new int16_t[num_frames * rec_channels];

	auto convert_to_int16 = [](const unsigned char *src, int samples, int bits_per_sample, int channels, int16_t *dst) -> bool
	{
		int total_values = samples * channels;
		switch (bits_per_sample)
		{
		case 8:
			for (int i = 0; i < total_values; ++i)
			{
				dst[i] = static_cast<int16_t>((static_cast<int>(src[i]) - 128) << 8);
			}
			break;
		case 16:
			memcpy(dst, src, total_values * sizeof(int16_t));
			break;
		case 24:
			for (int i = 0, j = 0; i < total_values; ++i, j += 3)
			{
				int32_t sample = static_cast<int32_t>(src[j]) |
								 (static_cast<int32_t>(src[j + 1]) << 8) |
								 (static_cast<int32_t>(src[j + 2]) << 16);
				if (sample & 0x800000)
					sample |= ~0xFFFFFF;
				dst[i] = static_cast<int16_t>(sample >> 8);
			}
			break;
		case 32:
			for (int i = 0, j = 0; i < total_values; ++i, j += 4)
			{
				int32_t sample = static_cast<int32_t>(src[j]) |
								 (static_cast<int32_t>(src[j + 1]) << 8) |
								 (static_cast<int32_t>(src[j + 2]) << 16) |
								 (static_cast<int32_t>(src[j + 3]) << 24);
				dst[i] = static_cast<int16_t>(sample >> 16);
			}
			break;
		default:
			return false;
		}
		return true;
	};

	cout << "processing audio frames ..." << endl;
	ProgressBar bar;
	while (current++ < total)
	{
		if (wav_read_data(h_ref, ref_tmp, bytes_per_frame_ref_in) <= 0){
			break;
		}

		if (wav_read_data(h_rec, aec_tmp, bytes_per_frame_rec_in) <= 0){
			break;
		}

		if (!convert_to_int16(ref_tmp, num_frames, ref_bits_per_sample, ref_channels, ref_converted) ||
			!convert_to_int16(aec_tmp, num_frames, rec_bits_per_sample, rec_channels, aec_converted))
		{
			cerr << "unsupported bits per sample, only 8/16/24/32 are supported" << endl;
			break;
		}

		ref_frame.UpdateFrame(0, ref_converted, num_frames, sample_rate, AudioFrame::kNormalSpeech, AudioFrame::kVadActive, ref_channels);
		aec_frame.UpdateFrame(0, aec_converted, num_frames, sample_rate, AudioFrame::kNormalSpeech, AudioFrame::kVadActive, rec_channels);

		ref_audio->CopyFrom(&ref_frame);
		aec_audio->CopyFrom(&aec_frame);

		// Convert audio signals from time domain to frequency domain
		ref_audio->SplitIntoFrequencyBands();
		aec_audio->SplitIntoFrequencyBands();

		// Analyze reference/microphone signals in frequency domain
		echo_controler->AnalyzeRender(ref_audio.get());
		echo_controler->AnalyzeCapture(aec_audio.get());

		// Apply high pass filter
		hp_filter->Process(aec_audio.get(), true);
		// Set audio buffer delay, assume reference/microphone streams to be synchronized
		echo_controler->SetAudioBufferDelay(0);

		// Echo cancellation, synthesize time domain AEC signal
		echo_controler->ProcessCapture(aec_audio.get(), aec_linear_audio.get(), false);
		aec_audio->MergeFrequencyBands();

		// Get postprocessed AEC output audio frame
		aec_audio->CopyTo(&aec_frame);
		memcpy(aec_out_tmp, aec_frame.data(), bytes_per_frame_rec_out);
		wav_write_data(h_out, aec_out_tmp, bytes_per_frame_rec_out);

		// Get linear AEC output audio frame
		aec_linear_audio->CopyTo(&aec_frame);
		memcpy(aec_out_tmp, aec_frame.data(), bytes_per_frame_rec_out);
		wav_write_data(h_linear_out, aec_out_tmp, bytes_per_frame_rec_out);

		bar.print_bar(current * 1.f / total);
	}
	std::cout << std::endl;
	delete[] ref_tmp;
	delete[] aec_tmp;
	delete[] aec_out_tmp;
	delete[] ref_converted;
	delete[] aec_converted;

	wav_read_close(h_ref);
	wav_read_close(h_rec);
	wav_write_close(h_out);
	wav_write_close(h_linear_out);

	return 0;
}
