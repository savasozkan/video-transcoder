/*!
**************************************************************************************
 * \file VideoTranscoder.h

 * \author
 *    - Savas Ozkan
 *************************************************************************************
 */

#pragma once

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfiltergraph.h"
#include "libavfilter/avcodec.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavcodec/avfft.h"
}

#include <iostream>
#include <auto_ptr.h>
#include <string>
#include <vector>

#include <exception>

/*!
 * This class mainly comprises an algorithm which transcodes an input media to an output media by
 * letting user to make changes on output media's configurations according to his/her demand.
 * Most of the code in this class is taken from an example that is given in
 * "http://ffmpeg.org/doxygen/trunk/transcoding_8c-example.html".However, due to the fact that
 * this example has immense number of bugs particularly on lack of audio and video formats encoding,
 * the code is modified and most of the bugs are solved. Furthermore, the trancoding example
 * is written within class structure which ease to use.
 *
 * There might still be some more bugs even in this code. If you encounter any of those, please contact with
 * the author via "http://www.savasozkan.com".
 */

using namespace std;

/***************/
/* Error Class */
/***************/

class Error : public exception
{
public:
	explicit Error(const string& message) throw() { m_message = message; }
	virtual ~Error() throw() {}
	virtual const char* what() const throw() { return m_message.c_str() ; }

private:
	string m_message;
};

/**************************/
/* VideoTranscoder Class */
/**************************/

class VideoTranscoder
{
public:

	struct st_filtering_context
	{
		AVFilterContext *m_buffersink_ctx;
		AVFilterContext *m_buffersrc_ctx;
		AVFilterGraph *m_filter_graph;
	};

	VideoTranscoder();
	virtual ~VideoTranscoder();

	void transcode(string pth_input_media, string pth_output_media);

private:

	int open_input_file(string pth_media);
	int open_output_file(string pth_media);
	void input_video_properties();

	int init_filters();
	int init_filter(st_filtering_context* f_ctx, AVCodecContext *dec_ctx, AVCodecContext *enc_ctx, const char *filter_spec);

	bool find_next_packet(AVPacket& packet);
	bool decode_packet(AVPacket& packet);
	int decode_media(AVFormatContext* context, AVPacket& packet, AVFrame* dec_frame, int& b_frame);

	bool encode_frame(int stream_index);
	int encode_media(AVFormatContext* context, AVPacket& packet, AVFrame* dec_frame, int stream_index, int& b_frame);
	int filter_encode_write_frame(AVFrame *frame, unsigned int stream_index);
	int encode_write_frame(AVFrame *filt_frame, int stream_index, int& b_frame);
	void aac_packet_filter(int stream_index, AVPacket& packet);

	int flush_encoder(unsigned int stream_index);
	int decode_frame_in_buffer(int stream_index);

	int time_to_frame(int stream_index, double time);
	double global_time_to_seconds(int64_t global_time) const;
	int64_t stream_time_to_global_time(AVRational time_base, int64_t nStreamTime) const;
	int64_t seconds_to_global_time(double seconds) const;

	void free_transcode_buffer();
	void free_open_buffer();

	AVFormatContext* m_ifmt_ctx;
	AVFormatContext* m_ofmt_ctx;
	st_filtering_context* m_filter_ctx;

	auto_ptr<AVPacket> m_packet;
	AVFrame* m_dec_frame;

	vector<double> m_v_duration;
	vector<int> m_v_numof_frames;
	vector<vector<int64_t> > m_v_timestamps;
};
