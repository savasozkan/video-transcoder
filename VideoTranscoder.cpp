/*!
**************************************************************************************
 * \file VideoTranscoder.cpp

 * \author
 *    - Savas Ozkan
 *************************************************************************************
 */

#include "VideoTranscoder.h"

VideoTranscoder::VideoTranscoder()
{
	m_packet = auto_ptr<AVPacket>(new AVPacket()) ;
	av_init_packet(m_packet.get()) ;

	m_dec_frame = av_frame_alloc();
	if(not m_dec_frame)
		throw Error("[VideoTranscoder] AvFrame can't be allocated");

	m_ifmt_ctx   = NULL;
	m_ofmt_ctx   = NULL;
	m_filter_ctx = NULL;
}

VideoTranscoder::~VideoTranscoder()
{
	av_free_packet(m_packet.get());
	avcodec_free_frame(&m_dec_frame);
	free_transcode_buffer();
}

void VideoTranscoder::transcode(string pth_input_media, string pth_output_media)
{
	av_register_all();
	avfilter_register_all();

	if(open_input_file(pth_input_media) <0)   throw Error("Error occurred during input media opening.");
	if(open_output_file(pth_output_media) <0) throw Error("Error occurred during output media opening.");
	if(init_filters() <0)                     throw Error("Filter can't be allocated.");

	while(true)
	{
		if(not find_next_packet(*m_packet)) break;

		int stream_index = m_packet->stream_index;
		AVMediaType type = m_ifmt_ctx->streams[stream_index]->codec->codec_type;

		if(not decode_packet(*m_packet)) continue;

		m_dec_frame->pts = av_rescale_q(av_frame_get_best_effort_timestamp(m_dec_frame),
										m_ifmt_ctx->streams[stream_index]->codec->time_base,
					 	                m_ofmt_ctx->streams[stream_index]->codec->time_base);

		//todo To make changes in avframe, append your code here.

		if(not encode_frame(stream_index))
			throw Error("Error occurred during encoding current frame.");
	}

	for(int i=0; i<int(m_ifmt_ctx->nb_streams); i++)
	{
		if(not m_filter_ctx[i].m_filter_graph) continue;
		AVMediaType type = m_ifmt_ctx->streams[i]->codec->codec_type;

		while(decode_frame_in_buffer(i) == 1)
		{
			if(not encode_frame(i)) break;
		}

		if(filter_encode_write_frame(NULL, i) < 0) break;
		if(flush_encoder(i) < 0) break;
	}

	av_write_trailer(m_ofmt_ctx);
}

int VideoTranscoder::open_input_file(string pth_media)
{
	int ret;
	unsigned int i;
	m_ifmt_ctx = NULL;

	if((ret = avformat_open_input(&m_ifmt_ctx, pth_media.c_str(), NULL, NULL)) < 0) return ret;
	if((ret = avformat_find_stream_info(m_ifmt_ctx, NULL)) < 0)             return ret;

	for (i = 0; i < m_ifmt_ctx->nb_streams; i++)
	{
		AVStream* stream          = m_ifmt_ctx->streams[i];
		AVCodecContext* codec_ctx = stream->codec;

		if(codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO or codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			ret = avcodec_open2(codec_ctx, avcodec_find_decoder(codec_ctx->codec_id), NULL);
			if(ret < 0) return ret;
		}
	}

	input_video_properties();

	return 0;
}

int VideoTranscoder::open_output_file(string pth_media)
{
	AVStream *out_stream;
	AVStream *in_stream;
	AVCodecContext *dec_ctx;
	AVCodec *encoder;

	int ret;
	unsigned int i;
	m_ofmt_ctx = NULL;
	avformat_alloc_output_context2(&m_ofmt_ctx, NULL, NULL, pth_media.c_str());

	if(!m_ofmt_ctx)
		return AVERROR_UNKNOWN;

	for(i = 0; i < m_ifmt_ctx->nb_streams; i++)
	{
		out_stream = avformat_new_stream(m_ofmt_ctx, NULL);
		if(!out_stream)
			return AVERROR_UNKNOWN;

		in_stream = m_ifmt_ctx->streams[i];
		dec_ctx   = in_stream->codec;

		if(dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO or dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			encoder = avcodec_find_encoder(dec_ctx->codec_id); //

			if(!encoder)
				return AVERROR_INVALIDDATA;

			m_ofmt_ctx->streams[i]->codec = avcodec_alloc_context3(encoder);

			if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
			{
				out_stream->codec->height              = dec_ctx->height;
				out_stream->codec->width               = dec_ctx->width;
				out_stream->codec->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;

				out_stream->codec->pix_fmt             = encoder->pix_fmts[0];
				out_stream->codec->time_base           = dec_ctx->time_base;
				out_stream->codec->qcompress           = dec_ctx->qcompress;
				out_stream->codec->bit_rate            = dec_ctx->bit_rate;
				out_stream->codec->gop_size            = dec_ctx->gop_size;
			}
			else
			{
				out_stream->codec->sample_rate    = dec_ctx->sample_rate;
				out_stream->codec->channel_layout = dec_ctx->channel_layout;
				out_stream->codec->channels       = av_get_channel_layout_nb_channels(out_stream->codec->channel_layout);

				out_stream->codec->sample_fmt     = encoder->sample_fmts[0];
				out_stream->codec->time_base      = dec_ctx->time_base;
			}

			ret = avcodec_open2(m_ofmt_ctx->streams[i]->codec, encoder, NULL);
			if(ret < 0)
				return ret;
		}
		else
		{
			throw Error("Unknown stream format.");
		}

		if(m_ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
			m_ofmt_ctx->streams[i]->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}

	if(!(m_ofmt_ctx->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&m_ofmt_ctx->pb, pth_media.c_str(), AVIO_FLAG_WRITE);
		if (ret < 0)
			return ret;
	}

	ret = avformat_write_header(m_ofmt_ctx, NULL);
	if(ret < 0)
		return ret;

	return 0;
}

void VideoTranscoder::input_video_properties()
{
	m_v_duration.clear();     m_v_duration.resize(m_ifmt_ctx->nb_streams);
	m_v_numof_frames.clear(); m_v_numof_frames.resize(m_ifmt_ctx->nb_streams);
	m_v_timestamps.clear();   m_v_timestamps.resize(m_ifmt_ctx->nb_streams);

	for(int s=0; s<int(m_ifmt_ctx->nb_streams); s++)
	{
		AVStream* stream = m_ifmt_ctx->streams[s];

		m_v_duration[s]     = global_time_to_seconds(stream_time_to_global_time(stream->time_base, stream->duration));
		m_v_numof_frames[s] = stream->nb_frames;

		for(int i=0; i<stream->nb_index_entries; i+=1)
		{
			m_v_timestamps[s].push_back( stream_time_to_global_time(stream->time_base, stream->index_entries[i].timestamp - stream->index_entries[0].timestamp) );
		}
	}
}

int VideoTranscoder::init_filters()
{
	const char *filter_spec;
	unsigned int i;
	int ret;

	m_filter_ctx = (st_filtering_context *)av_malloc_array(m_ifmt_ctx->nb_streams, sizeof(*m_filter_ctx));

	if(!m_filter_ctx)
		return -1;

	for(i = 0; i <m_ifmt_ctx->nb_streams; i++)
	{
		m_filter_ctx[i].m_buffersrc_ctx  = NULL;
		m_filter_ctx[i].m_buffersink_ctx = NULL;
		m_filter_ctx[i].m_filter_graph   = NULL;
		if( not (m_ifmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO or
			     m_ifmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) )
			continue;

		if(m_ifmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) filter_spec = "null";
		else filter_spec = "anull";

		ret = init_filter(&m_filter_ctx[i], m_ifmt_ctx->streams[i]->codec, m_ofmt_ctx->streams[i]->codec, filter_spec);
		if(ret) return ret;
	}

	return 0;
}

int VideoTranscoder::init_filter(st_filtering_context* f_ctx, AVCodecContext *dec_ctx,
								 AVCodecContext *enc_ctx, const char *filter_spec)
{
	char args[512];
	int ret = 0;

	AVFilter *buffersrc             = NULL;
	AVFilter *buffersink            = NULL;
	AVFilterContext *buffersrc_ctx  = NULL;
	AVFilterContext *buffersink_ctx = NULL;

	AVFilterInOut *outputs      = avfilter_inout_alloc();
	AVFilterInOut *inputs       = avfilter_inout_alloc();
	AVFilterGraph *filter_graph = avfilter_graph_alloc();

	if((not outputs) or (not inputs) or (not filter_graph))
	{
		avfilter_inout_free(&inputs);
		avfilter_inout_free(&outputs);
		return AVERROR(ENOMEM);
	}

	if(dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
	{
		buffersrc  = avfilter_get_by_name("buffer");
		buffersink = avfilter_get_by_name("buffersink");
		if( (not buffersrc) or (not buffersink) )
		{
			avfilter_inout_free(&inputs);
			avfilter_inout_free(&outputs);
			return AVERROR_UNKNOWN;
		}

		snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
		dec_ctx->time_base.num, dec_ctx->time_base.den,
		dec_ctx->sample_aspect_ratio.num,
		dec_ctx->sample_aspect_ratio.den);

		ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
		if(ret < 0)
		{
			avfilter_inout_free(&inputs);
			avfilter_inout_free(&outputs);
			return ret;
		}

		ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
		if(ret < 0)
		{
			avfilter_inout_free(&inputs);
			avfilter_inout_free(&outputs);
			return ret;
		}

		ret = av_opt_set_bin(buffersink_ctx, "pix_fmts", (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt), AV_OPT_SEARCH_CHILDREN);
		if(ret < 0)
		{
			avfilter_inout_free(&inputs);
			avfilter_inout_free(&outputs);
			return ret;
		}
	}
	else if(dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
	{
		buffersrc  = avfilter_get_by_name("abuffer");
		buffersink = avfilter_get_by_name("abuffersink");
		if( (not buffersrc) or (not buffersink) )
		{
			avfilter_inout_free(&inputs);
			avfilter_inout_free(&outputs);
			return ret;
		}

		if(not dec_ctx->channel_layout)
			dec_ctx->channel_layout = av_get_default_channel_layout(dec_ctx->channels);

		snprintf(args, sizeof(args),
		"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
		 dec_ctx->time_base.num, dec_ctx->time_base.den, dec_ctx->sample_rate,
		 av_get_sample_fmt_name(dec_ctx->sample_fmt),
		 dec_ctx->channel_layout);

		ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
		if(ret < 0)
		{
			avfilter_inout_free(&inputs);
			avfilter_inout_free(&outputs);
			return ret;
		}

		ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
		if(ret < 0)
		{
			avfilter_inout_free(&inputs);
			avfilter_inout_free(&outputs);
			return ret;
		}

		ret = av_opt_set_bin(buffersink_ctx, "sample_fmts", (uint8_t*)&enc_ctx->sample_fmt, sizeof(enc_ctx->sample_fmt), AV_OPT_SEARCH_CHILDREN);
		if(ret < 0)
		{
			avfilter_inout_free(&inputs);
			avfilter_inout_free(&outputs);
			return ret;
		}

		ret = av_opt_set_bin(buffersink_ctx, "channel_layouts", (uint8_t*)&enc_ctx->channel_layout, sizeof(enc_ctx->channel_layout), AV_OPT_SEARCH_CHILDREN);
		if(ret < 0)
		{
			avfilter_inout_free(&inputs);
			avfilter_inout_free(&outputs);
			return ret;
		}

		ret = av_opt_set_bin(buffersink_ctx, "sample_rates", (uint8_t*)&enc_ctx->sample_rate, sizeof(enc_ctx->sample_rate), AV_OPT_SEARCH_CHILDREN);
		if(ret < 0)
		{
			avfilter_inout_free(&inputs);
			avfilter_inout_free(&outputs);
			return ret;
		}
	}
	else
	{
		avfilter_inout_free(&inputs);
		avfilter_inout_free(&outputs);
		return ret;
	}

	outputs->name       = av_strdup("in");
	outputs->filter_ctx = buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;
	inputs->name        = av_strdup("out");
	inputs->filter_ctx  = buffersink_ctx;
	inputs->pad_idx     = 0;
	inputs->next        = NULL;

	if( (not outputs->name) or (not inputs->name) )
	{
		avfilter_inout_free(&inputs);
		avfilter_inout_free(&outputs);
		return ret;
	}

	if(avfilter_graph_parse_ptr(filter_graph, filter_spec, &inputs, &outputs, NULL) < 0 or
	   avfilter_graph_config(filter_graph, NULL) < 0)
	{
		avfilter_inout_free(&inputs);
		avfilter_inout_free(&outputs);
		return ret;
	}

	f_ctx->m_buffersrc_ctx  = buffersrc_ctx;
	f_ctx->m_buffersink_ctx = buffersink_ctx;
	f_ctx->m_filter_graph   = filter_graph;

	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	return ret;
}

bool VideoTranscoder::find_next_packet(AVPacket& packet)
{
	av_init_packet(&packet);
	while(av_read_frame(m_ifmt_ctx, &packet) >= 0)
	{
		if(packet.pts >= 0)
		{
			av_packet_rescale_ts(&packet, m_ifmt_ctx->streams[packet.stream_index]->time_base,
								 m_ifmt_ctx->streams[packet.stream_index]->codec->time_base);

			return true;
		}

		av_free_packet(&packet) ;
		av_init_packet(&packet) ;
	}

	return false;
}

bool VideoTranscoder::decode_packet(AVPacket& packet)
{
	int bytes_decoded = 0;
	int b_frame = 0;

	// Get rid of old one.
	avcodec_free_frame(&m_dec_frame);

	while (packet.size > 0)
	{
		// Allocate new av_frame.
		m_dec_frame = av_frame_alloc();
		bytes_decoded = decode_media(m_ifmt_ctx, packet, m_dec_frame, b_frame);

	    if(bytes_decoded < 0)
	        throw Error("[VideoTrancoders]This is an unusual case. So, please contact with the developer.");

	    packet.size -= bytes_decoded;
	    packet.data += bytes_decoded;


	    if(b_frame) return true;
	}

	return false;
}

int VideoTranscoder::decode_media(AVFormatContext* context, AVPacket& packet, AVFrame* dec_frame, int& b_frame)
{
	int bytes_decoded = 0;
	int stream_index  = packet.stream_index;

	try
	{
		switch(context->streams[stream_index]->codec->codec_type)
		{
		case AVMEDIA_TYPE_VIDEO:
		{
			bytes_decoded = avcodec_decode_video2(context->streams[stream_index]->codec, dec_frame, &b_frame, &packet);
			break;
		}
		case AVMEDIA_TYPE_AUDIO:
		{
			bytes_decoded = avcodec_decode_audio4(context->streams[stream_index]->codec, dec_frame, &b_frame, &packet);
			break;
		}
		default:
			throw Error("[VideoTrancoder] VideoTranscoder only decodes video and audio formats.");
		}
	}
	catch(Error& e)
	{
		b_frame = false;
		bytes_decoded = 0;
	}

	return bytes_decoded;
}

bool VideoTranscoder::encode_frame(int stream_index)
{
	if( filter_encode_write_frame(m_dec_frame, stream_index) < 0 ) return false;

	return true;
}

int VideoTranscoder::encode_media(AVFormatContext* context, AVPacket& packet, AVFrame* dec_frame, int stream_index, int& b_frame)
{
	int bytes_decoded = 0;

	try
	{
		switch(context->streams[stream_index]->codec->codec_type)
		{
		case AVMEDIA_TYPE_VIDEO:
		{
			bytes_decoded = avcodec_encode_video2(context->streams[stream_index]->codec, &packet, dec_frame, &b_frame);
			break;
		}
		case AVMEDIA_TYPE_AUDIO:
		{
			bytes_decoded = avcodec_encode_audio2(context->streams[stream_index]->codec, &packet, dec_frame, &b_frame);
			break;
		}
		default:
			throw Error("[VideoTrancoder] VideoTranscoder only encode video and audio formats.");
		}
	}
	catch(Error& e)
	{
		b_frame = false;
		bytes_decoded = 0;
	}

	return bytes_decoded;
}

int VideoTranscoder::filter_encode_write_frame(AVFrame *frame, unsigned int stream_index)
{
	int ret, b_frame;
	AVFrame *filt_frame;

	ret = av_buffersrc_add_frame_flags(m_filter_ctx[stream_index].m_buffersrc_ctx, frame, 0);
	if(ret < 0)
		return ret;

	while(true)
	{
		filt_frame = av_frame_alloc();
		if(!filt_frame)
		{
			ret = AVERROR(ENOMEM);
			break;
		}

		ret = av_buffersink_get_frame(m_filter_ctx[stream_index].m_buffersink_ctx, filt_frame);
		if(ret < 0)
		{
			if(ret == AVERROR(EAGAIN) or ret == AVERROR_EOF) ret = 0;

			av_frame_free(&filt_frame);
			break;
		}

		filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
		ret = encode_write_frame(filt_frame, stream_index, b_frame);
		if(ret < 0) break;
	}

	return ret;
}

int VideoTranscoder::encode_write_frame(AVFrame *filt_frame, int stream_index, int& b_frame)
{
	int ret;
	AVPacket enc_pkt;

	enc_pkt.data = NULL;
	enc_pkt.size = 0;

	av_init_packet(&enc_pkt);
	ret = encode_media(m_ofmt_ctx, enc_pkt, filt_frame, stream_index, b_frame);
	av_frame_free(&filt_frame);

	if (ret < 0) return ret;
	if (!(b_frame)) return 0;

	enc_pkt.stream_index = stream_index;
	av_packet_rescale_ts(&enc_pkt, m_ofmt_ctx->streams[stream_index]->codec->time_base,
						 m_ofmt_ctx->streams[stream_index]->time_base);

	if(m_ofmt_ctx->streams[stream_index]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
		aac_packet_filter(stream_index, enc_pkt);

	ret = av_interleaved_write_frame(m_ofmt_ctx, &enc_pkt);
	av_free_packet(&enc_pkt);

	return ret;
}

void VideoTranscoder::aac_packet_filter(int stream_index, AVPacket& packet)
{
	if(m_ifmt_ctx->streams[stream_index]->codec->codec_id != AV_CODEC_ID_AAC) return;

	AVBitStreamFilterContext* bsfc = av_bitstream_filter_init("aac_adtstoasc");

    int err_val = av_bitstream_filter_filter(bsfc, m_ofmt_ctx->streams[stream_index]->codec,
    										 NULL, &packet.data, &packet.size,
											 packet.data, packet.size,
											 0);

    if(err_val < 0)
    	throw Error("[VideoTranscoder] Error occurred during filtering audio packet with aac.");

    av_bitstream_filter_close(bsfc);
}

int VideoTranscoder::flush_encoder(unsigned int stream_index)
{
	int err_val, b_frame;

	if(!(m_ofmt_ctx->streams[stream_index]->codec->codec->capabilities & CODEC_CAP_DELAY))
		return 0;

	while(true)
	{
		err_val = encode_write_frame(NULL, stream_index, b_frame);
		if(err_val < 0) break;
		if(!b_frame) return 0;
	}

	return err_val;
}

int VideoTranscoder::decode_frame_in_buffer(int stream_index)
{
	m_packet->data = NULL;
	m_packet->size = 0;
	int b_frame = 0;

	avcodec_free_frame(&m_dec_frame);
	m_dec_frame = av_frame_alloc();
	decode_media(m_ifmt_ctx, *m_packet, m_dec_frame, b_frame);

	if(b_frame and m_dec_frame->pkt_size >= 0) return 1;

	return -1;
}

int VideoTranscoder::time_to_frame(int stream_index, double time)
{
	if(time < 0.0) return 0 ;
	else if(time >= m_v_duration[stream_index]) return max(0, m_v_numof_frames[stream_index] - 1) ;

	int64_t global_time = seconds_to_global_time(time) ;
	vector<int64_t>& v_timestamp = m_v_timestamps[stream_index];

	int a = 0 ;
	int b = int(v_timestamp.size()) ;
	while(b - a > 1)
	{
		int m = (a + b) >> 1 ;
		if(v_timestamp[m] >= global_time) b = m ;
		if(v_timestamp[m] <= global_time) a = m ;
	}

	if(global_time - v_timestamp[a] < v_timestamp[b] - global_time and b < int(v_timestamp.size()) )
		return a ;
	else
		return b ;
}

double VideoTranscoder::global_time_to_seconds(int64_t global_time) const
{
	return double(global_time) / double(AV_TIME_BASE);
}

int64_t VideoTranscoder::stream_time_to_global_time(AVRational time_base, int64_t nStreamTime) const
{
	return av_rescale_q(nStreamTime, time_base, AV_TIME_BASE_Q);
}

int64_t VideoTranscoder::seconds_to_global_time(double seconds) const
{
	return seconds*double(AV_TIME_BASE);
}

void VideoTranscoder::free_transcode_buffer()
{
	for(int i=0; i<int(m_ifmt_ctx->nb_streams); i++)
	{
		if(m_ifmt_ctx)
			avcodec_close(m_ifmt_ctx->streams[i]->codec);

		if(m_ofmt_ctx)
		{
			if( (int(m_ofmt_ctx->nb_streams) > i) and
				m_ofmt_ctx->streams[i] and
				m_ofmt_ctx->streams[i]->codec)
				avcodec_close(m_ofmt_ctx->streams[i]->codec);
		}

		if(m_filter_ctx)
		{
			if(m_filter_ctx[i].m_filter_graph)
				avfilter_graph_free(&m_filter_ctx[i].m_filter_graph);
		}
	}

	if(m_filter_ctx) av_free(m_filter_ctx);
	if(m_ifmt_ctx)   avformat_close_input(&m_ifmt_ctx);
	if(m_ofmt_ctx and !(m_ofmt_ctx->oformat->flags & AVFMT_NOFILE))
	{
		avio_closep(&m_ofmt_ctx->pb);
		avformat_free_context(m_ofmt_ctx);
	}
}

void VideoTranscoder::free_open_buffer()
{
	for(int i=0; i<int(m_ifmt_ctx->nb_streams); i++)
		avcodec_close(m_ifmt_ctx->streams[i]->codec);

	avformat_close_input(&m_ifmt_ctx);
}
