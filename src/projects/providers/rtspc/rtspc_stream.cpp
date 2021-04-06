//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2021 AirenSoft. All rights reserved.
//
//==============================================================================

#include <base/info/application.h>
#include <modules/rtsp/header_fields/rtsp_header_fields.h>

#include "rtspc_stream.h"
#include "rtspc_provider.h"

#define OV_LOG_TAG "RtspcStream"

namespace pvd
{
	std::shared_ptr<RtspcStream> RtspcStream::Create(const std::shared_ptr<pvd::PullApplication> &application, 
		const uint32_t stream_id, const ov::String &stream_name,
		const std::vector<ov::String> &url_list)
	{
		info::Stream stream_info(*std::static_pointer_cast<info::Application>(application), StreamSourceType::RtspPull);

		stream_info.SetId(stream_id);
		stream_info.SetName(stream_name);

		auto stream = std::make_shared<RtspcStream>(application, stream_info, url_list);
		if (!stream->Start())
		{
			// Explicit deletion
			stream.reset();
			return nullptr;
		}

		return stream;
	}

	RtspcStream::RtspcStream(const std::shared_ptr<pvd::PullApplication> &application, const info::Stream &stream_info, const std::vector<ov::String> &url_list)
	: pvd::PullStream(application, stream_info)
	{
		_state = State::IDLE;

		for(auto &url : url_list)
		{
			auto parsed_url = ov::Url::Parse(url);
			if(parsed_url)
			{
				_url_list.push_back(parsed_url);
			}
		}

		if(!_url_list.empty())
		{
			_curr_url = _url_list[0];
			SetMediaSource(_curr_url->ToUrlString(true));
		}
	}

	RtspcStream::~RtspcStream()
	{
		Stop();
		Release();
	}

	std::shared_ptr<pvd::RtspcProvider> RtspcStream::GetRtspcProvider()
	{
		return std::static_pointer_cast<RtspcProvider>(_application->GetParentProvider());
	}

	bool RtspcStream::AddDepacketizer(uint8_t payload_type, cmn::MediaCodecId codec_id)
	{
		// Depacketizer
		auto depacketizer = RtpDepacketizingManager::Create(codec_id);
		if(depacketizer == nullptr)
		{
			logte("%s - Could not create depacketizer : codec_id(%d)", GetName().CStr(), static_cast<uint8_t>(codec_id));
			return false;
		}

		_depacketizers[payload_type] = depacketizer;

		return true;
	}

	std::shared_ptr<RtpDepacketizingManager> RtspcStream::GetDepacketizer(uint8_t payload_type)
	{
		auto it = _depacketizers.find(payload_type);
		if(it == _depacketizers.end())
		{
			return nullptr;
		}

		return it->second;
	}

	void RtspcStream::Release()
	{
	}

	bool RtspcStream::Start()
	{
		if(_state != State::IDLE)
		{
			return false;
		}

		ov::StopWatch stop_watch;

		stop_watch.Start();
		if(ConnectTo() == false)
		{
			return false;
		}
		_origin_request_time_msec = stop_watch.Elapsed();

		stop_watch.Update();

		if(RequestDescribe() == false)
		{
			return false;
		}

		if(RequestSetup() == false)
		{
			return false;
		}

		_origin_response_time_msec = stop_watch.Elapsed();

		return pvd::PullStream::Start();
	}

	bool RtspcStream::Play()
	{
		if (!RequestPlay())
		{
			return false;
		}

		// Stream was created completly 
		_stream_metrics = StreamMetrics(*std::static_pointer_cast<info::Stream>(PullStream::GetSharedPtr()));
		if(_stream_metrics != nullptr)
		{
			_stream_metrics->SetOriginRequestTimeMSec(_origin_request_time_msec);
			_stream_metrics->SetOriginResponseTimeMSec(_origin_response_time_msec);
		}

		return pvd::PullStream::Play();
	}

	bool RtspcStream::Stop()
	{
		// Already stopping
		if(_state != State::PLAYING)
		{
			return true;
		}
		
		if(!RequestStop())
		{
			// Force terminate 
			_state = State::ERROR;
		}

		_state = State::STOPPED;
	
		return pvd::PullStream::Stop();
	}

	bool RtspcStream::ConnectTo()
	{
		if(_state != State::IDLE && _state != State::ERROR)
		{
			return false;
		}

		logti("Requested url[%d] : %s", strlen(_curr_url->Source().CStr()), _curr_url->Source().CStr() );

		auto scheme = _curr_url->Scheme();
		if(scheme.UpperCaseString() != "RTSP")
		{
			_state = State::ERROR;
			logte("The scheme is not rtsp : %s", scheme.CStr());
			return false;
		}

		auto signalling_socket_pool = GetRtspcProvider()->GetSignallingSocketPool();
		if (signalling_socket_pool == nullptr)
		{
			// Provider is not initialized
			logte("Could not get socket from socket pool");
			return false;
		}

		// Connect
		_signalling_socket = signalling_socket_pool->AllocSocket();
		if ((_signalling_socket == nullptr) || (_signalling_socket->AttachToWorker() == false))
		{
			_state = State::ERROR;
			logte("To create client socket is failed.");
			
			_signalling_socket = nullptr;
			return false;
		}

		_signalling_socket->MakeBlocking();

		// 554 is default port of RTSP
		ov::SocketAddress socket_address(_curr_url->Host(), _curr_url->Port() == 0 ? 554 : _curr_url->Port());

		auto error = _signalling_socket->Connect(socket_address, 3000);
		if (error != nullptr)
		{
			_state = State::ERROR;
			logte("Cannot connect to server (%s) : %s:%d", error->GetMessage().CStr(), _curr_url->Host().CStr(), _curr_url->Port());
			return false;
		}

		_state = State::CONNECTED;

		return true;
	}

	bool RtspcStream::RequestDescribe()
	{
		if(_state != State::CONNECTED)
		{
			return false;
		}

		auto describe = std::make_shared<RtspMessage>(RtspMethod::DESCRIBE, GetNextCSeq(), _curr_url->ToUrlString(true));
		describe->AddHeaderField(std::make_shared<RtspHeaderField>(RtspHeaderFieldType::Accept,"application/sdp"));
		describe->AddHeaderField(std::make_shared<RtspHeaderField>(RtspHeaderFieldType::UserAgent, RTSP_USER_AGENT_NAME));
		
		if(SendRequestMessage(describe) == false)
		{
			_state = State::ERROR;
			logte("Could not request DESCIBE to RTSP server (%s)", _curr_url->ToUrlString().CStr());
			return false;
		}

		auto reply = ReceiveResponse(describe->GetCSeq(), 3000);
		
		if(reply == nullptr)
		{
			_state = State::ERROR;
			logte("No response(CSeq : %u) was received from the rtsp server(%s)", describe->GetCSeq(), _curr_url->ToUrlString().CStr());
			return false;
		}
		else if(reply->GetStatusCode() != 200)
		{
			_state = State::ERROR;
			logte("Rtsp server(%s) rejected the describe request : %d(%s)", _curr_url->ToUrlString().CStr(), reply->GetStatusCode(), reply->GetReasonPhrase().CStr());
			return false;
		}

		// Content-Base
		auto content_base_field = reply->GetHeaderField(RtspHeaderField::FieldTypeToString(RtspHeaderFieldType::ContentBase));
		if(content_base_field != nullptr)
		{
			_content_base = content_base_field->GetValue();
		}

		// Session
		auto session_field = reply->GetHeaderFieldAs<RtspHeaderSessionField>(RtspHeaderField::FieldTypeToString(RtspHeaderFieldType::Session));
		if(session_field == nullptr)
		{
			_state = State::ERROR;
			logte("There is no Session field in the header of describe reply. Url(%s) CSeq(%d)", _curr_url->ToUrlString().CStr(), describe->GetCSeq());
			return false;
		}

		// TODO(Getroot) : Parse session
		// Session  = "Session" ":" session-id [ ";" "timeout" "=" delta-seconds ]
		_rtsp_session_id = session_field->GetSessionId();
		// timeout
		[[maybe_unused]] auto timeout_delta_seconds = session_field->GetTimeoutDeltaSeconds();

		if(reply->GetBody() == nullptr)
		{
			_state = State::ERROR;
			logte("There is no SDP in the describe response. Url(%s) CSeq(%d)", _curr_url->ToUrlString().CStr(), describe->GetCSeq());
			return false;
		}

		// Parse SDP to add track information
		SessionDescription	sdp;
		if(sdp.FromString(reply->GetBody()->ToString()) == false)
		{
			_state = State::ERROR;
			logte("Parsing of SDP received from rtsp url (%s)failed. ", _curr_url->ToUrlString().CStr());
			return false;
		}

		_rtp_rtcp = std::make_shared<RtpRtcp>(RtpRtcpInterface::GetSharedPtr());

		auto media_desc_list = sdp.GetMediaList();
		for(const auto &media_desc : media_desc_list)
		{
			auto first_payload = media_desc->GetFirstPayload();
			if(first_payload == nullptr)
			{
				logte("Failed to get the first Payload type of peer sdp");
				return false;
			}

			if(media_desc->GetMediaType() == MediaDescription::MediaType::Video)
			{
				_video_control = media_desc->GetControl();
				if(_video_control.IsEmpty())
				{
					_state = State::ERROR;
					logte("Could not get control attribute in (%s) ", _curr_url->ToUrlString().CStr());
					return false;
				}

				_video_control_url = GenerateControlUrl(_video_control);
				if(_video_control_url.IsEmpty())
				{
					_state = State::ERROR;
					logte("Could not make control url with (%s) ", _video_control.CStr());
					return false;
				}

				_video_payload_type = first_payload->GetId();
				
				auto codec = first_payload->GetCodec();
				auto timebase = first_payload->GetCodecRate();
				
				auto video_track = std::make_shared<MediaTrack>();

				video_track->SetId(first_payload->GetId());
				video_track->SetMediaType(cmn::MediaType::Video);

				if(codec == PayloadAttr::SupportCodec::H264)
				{
					video_track->SetCodecId(cmn::MediaCodecId::H264);
				}
				else if(codec == PayloadAttr::SupportCodec::VP8)
				{
					video_track->SetCodecId(cmn::MediaCodecId::Vp8);
				}
				else
				{
					logte("%s - Unsupported video codec  : %s", GetName().CStr(), first_payload->GetCodecParams().CStr());
					return false;
				}

				video_track->SetTimeBase(1, timebase);
				video_track->SetVideoTimestampScale(1.0);
				
				if(AddDepacketizer(_video_payload_type, video_track->GetCodecId()) == false)
				{
					return false;
				}

				AddTrack(video_track);

				_rtp_rtcp->AddRtpReceiver(_video_payload_type, video_track);
			}
			else if(media_desc->GetMediaType() == MediaDescription::MediaType::Audio)
			{
				// Later
				// _audio_control = media_desc->GetControl();
			}
		}

		// Single node
		_rtp_rtcp->RegisterUpperNode(nullptr);
		_rtp_rtcp->RegisterLowerNode(nullptr);
		_rtp_rtcp->Start();

		_state = State::DESCRIBED;

		return true;
	}

	bool RtspcStream::RequestSetup()
	{
		if(_state != State::DESCRIBED)
		{
			return false;
		}

		int interleaved_channel = 0;

		for(const auto &it : GetTracks())
		{
			auto track = it.second;

			ov::String setup_url;

			if(track->GetMediaType() == cmn::MediaType::Video)
			{
				setup_url = _video_control_url;
			}
			else
			{
				setup_url = _audio_control_url;
			}

			auto setup = std::make_shared<RtspMessage>(RtspMethod::SETUP, GetNextCSeq(), setup_url);

			// Now RtspcStream only supports RTP/AVP/TCP;unicast/interleaved(rtp+rtcp)
			// The chennel id can be used for demuxing, but since it is already demuxing in a different way, it is not saved.
			setup->AddHeaderField(std::make_shared<RtspHeaderField>(RtspHeaderFieldType::Transport,
									ov::String::FormatString("RTP/AVP/TCP;unicast;interleaved=%d-%d", interleaved_channel, interleaved_channel+1)));
			interleaved_channel += 2;
			setup->AddHeaderField(std::make_shared<RtspHeaderField>(RtspHeaderFieldType::Session, _rtsp_session_id));
			setup->AddHeaderField(std::make_shared<RtspHeaderField>(RtspHeaderFieldType::UserAgent, RTSP_USER_AGENT_NAME));
			
			if(SendRequestMessage(setup) == false)
			{
				_state = State::ERROR;
				logte("Could not request DESCIBE to RTSP server (%s)", _curr_url->ToUrlString().CStr());
				return false;
			}

			auto reply = ReceiveResponse(setup->GetCSeq(), 3000);
			if(reply == nullptr)
			{
				_state = State::ERROR;
				logte("No response(CSeq : %u) was received from the rtsp server(%s)", setup->GetCSeq(), _curr_url->ToUrlString().CStr());
				return false;
			}
			else if(reply->GetStatusCode() != 200)
			{
				_state = State::ERROR;
				logte("Rtsp server(%s) rejected the describe request : %d(%s)", _curr_url->ToUrlString().CStr(), reply->GetStatusCode(), reply->GetReasonPhrase().CStr());
				return false;
			}
		}

		return true;
	}

	bool RtspcStream::RequestPlay()
	{
		if(_state != State::DESCRIBED)
		{
			return false;
		}

		auto play = std::make_shared<RtspMessage>(RtspMethod::PLAY, GetNextCSeq(), _curr_url->ToUrlString(true));

		play->AddHeaderField(std::make_shared<RtspHeaderField>(RtspHeaderFieldType::Session, _rtsp_session_id));
		play->AddHeaderField(std::make_shared<RtspHeaderField>(RtspHeaderFieldType::UserAgent, RTSP_USER_AGENT_NAME));
		
		if(SendRequestMessage(play) == false)
		{
			_state = State::ERROR;
			logte("Could not request DESCIBE to RTSP server (%s)", _curr_url->ToUrlString().CStr());
			return false;
		}

		auto reply = ReceiveResponse(play->GetCSeq(), 3000);
		
		if(reply == nullptr)
		{
			_state = State::ERROR;
			logte("No response(CSeq : %u) was received from the rtsp server(%s)", play->GetCSeq(), _curr_url->ToUrlString().CStr());
			return false;
		}
		else if(reply->GetStatusCode() != 200)
		{
			_state = State::ERROR;
			logte("Rtsp server(%s) rejected the describe request : %d(%s)", _curr_url->ToUrlString().CStr(), reply->GetStatusCode(), reply->GetReasonPhrase().CStr());
			return false;
		}

		_state = State::PLAYING;

		return true;
	}

	bool RtspcStream::RequestStop()
	{
		if (_state != State::PLAYING)
		{
			return false;
		}

		auto teardown = std::make_shared<RtspMessage>(RtspMethod::TEARDOWN, GetNextCSeq(), _curr_url->ToUrlString(true));

		teardown->AddHeaderField(std::make_shared<RtspHeaderField>(RtspHeaderFieldType::Session, _rtsp_session_id));
		teardown->AddHeaderField(std::make_shared<RtspHeaderField>(RtspHeaderFieldType::UserAgent, RTSP_USER_AGENT_NAME));
		
		if(SendRequestMessage(teardown) == false)
		{
			_state = State::ERROR;
			logte("Could not request Stop to RTSP server (%s)", _curr_url->ToUrlString().CStr());
			return false;
		}

		auto reply = ReceiveResponse(teardown->GetCSeq(), 3000);
		if(reply == nullptr)
		{
			_state = State::ERROR;
			logte("No response(CSeq : %u) was received from the rtsp server(%s)", teardown->GetCSeq(), _curr_url->ToUrlString().CStr());
			return false;
		}
		else if(reply->GetStatusCode() != 200)
		{
			_state = State::ERROR;
			logte("Rtsp server(%s) rejected the describe request : %d(%s)", _curr_url->ToUrlString().CStr(), reply->GetStatusCode(), reply->GetReasonPhrase().CStr());
			return false;
		}

		_state = State::STOPPING;

		return true;
	}

	int32_t RtspcStream::GetNextCSeq()
	{
		return _cseq++;
	}

	bool RtspcStream::SubscribeResponse(const std::shared_ptr<RtspMessage> &request_message)
	{
		std::lock_guard<std::mutex> lock(_response_subscriptions_lock);
		_response_subscriptions.emplace(request_message->GetCSeq(), std::make_shared<ResponseSubscription>(request_message));
		return true;
	}

	std::shared_ptr<RtspcStream::ResponseSubscription> RtspcStream::PopResponseSubscription(uint32_t cseq)
	{
		std::lock_guard<std::mutex> lock(_response_subscriptions_lock);
		auto it = _response_subscriptions.find(cseq);
		if(it == _response_subscriptions.end())
		{
			logte("There is no request message to receive a response. (CSeq : %u)", cseq);
			return nullptr;
		}

		auto request_response = it->second;

		// Remove waited request message
		_response_subscriptions.erase(it);

		return request_response;
	}

	bool RtspcStream::SendRequestMessage(const std::shared_ptr<RtspMessage> &message)
	{
		// Add to RequestedMap to receive reply
		SubscribeResponse(message);

		// Send
		return _signalling_socket->Send(message->GetMessage());
	}

	std::shared_ptr<RtspMessage> RtspcStream::ReceiveResponse(uint32_t cseq, uint64_t timeout_ms)
	{
		auto request_response = PopResponseSubscription(cseq);
		if(request_response == nullptr)
		{
			return nullptr;
		}
	
		// When the stream is playing, another thread receives a message and notifies it.
		if(GetState() == State::PLAYING)
		{
			return request_response->WaitForResponse(timeout_ms);
		}	
		// Otherwise, the response must be received directly from the socket.
		else
		{
			auto reply = ReceiveMessage(timeout_ms);
			// If the stream is not in the playing state, the client cannot receive unexpected CSeq.
			if(reply == nullptr)
			{	
				// timed out
				return nullptr;
			}
			else if(reply->GetCSeq() != cseq)
			{
				logte("Unexpected CSeq : %u (expected : %u)", reply->GetCSeq(), cseq);
				return nullptr;
			}

			return reply;
		}

		return nullptr;
	}

	std::shared_ptr<RtspMessage> RtspcStream::ReceiveMessage(int64_t timeout_msec)
	{
		ov::StopWatch stop_watch;

		stop_watch.Start();
		while(true)
		{
			if(ReceivePacket(false, timeout_msec) == false)
			{
				return nullptr;
			}

			if(_rtsp_demuxer.IsAvailableMessage())
			{
				return _rtsp_demuxer.PopMessage();
			}

			if(stop_watch.IsElapsed(timeout_msec))
			{
				return nullptr;
			}
		}

		return nullptr;
	}

	bool RtspcStream::ReceivePacket(bool non_block, int64_t timeout_msec)
	{
		uint8_t	buffer[65535];
		size_t read_bytes = 0ULL;

		if(non_block == false && timeout_msec != 0)
		{
			struct timeval tv = {timeout_msec/1000, (timeout_msec%1000)*1000};
			_signalling_socket->SetRecvTimeout(tv);
		}

		auto error = _signalling_socket->Recv(buffer, 65535, &read_bytes, non_block);
		if(read_bytes == 0)
		{
			if (error != nullptr)
			{
				logte("[%s/%s] An error occurred while receiving packet: %s", GetApplicationName(), GetName().CStr(), error->ToString().CStr());
				_state = State::ERROR;
				return false;
			}
			else
			{
				if(non_block == true)
				{
					// retry later
					return true;
				}
				else
				{
					// timeout
					return false;
				}
			}
		}

		// Since the response to the Play request and part of the interleaved data can be received at once, 
		// use _rtsp_demuxer to prevent the packet from being missed, regardless of the current state.
		if(_rtsp_demuxer.AppendPacket(buffer, read_bytes) == false)
		{
			logte("[%s/%s] An error occurred while parsing packet: Invalid packet", GetApplicationName(), GetName().CStr());
			return false;
		}

		return true;
	}

	int RtspcStream::GetFileDescriptorForDetectingEvent()
	{
		return _signalling_socket->GetSocket().GetNativeHandle();
	}

	PullStream::ProcessMediaResult RtspcStream::ProcessMediaPacket()
	{
		// Receive Packet
		auto result = ReceivePacket(true);
		if(result == false)
		{
			Stop();
			logte("%s/%s(%u) - Could not receive packet : err(%d)", GetApplicationInfo().GetName().CStr(), GetName().CStr(), GetId(), static_cast<uint8_t>(result));
			_state = State::ERROR;
			return ProcessMediaResult::PROCESS_MEDIA_FAILURE;
		}

		while(true)
		{
			if(_rtsp_demuxer.IsAvailableMessage())
			{
				auto rtsp_message = _rtsp_demuxer.PopMessage();
				if(rtsp_message->GetMessageType() == RtspMessageType::RESPONSE)
				{
					// Find Request
					auto subscription = PopResponseSubscription(rtsp_message->GetCSeq());
					if(subscription == nullptr)
					{
						// Response subscription is probably already timed out
						// Is Network very slow? or server error?
						continue;
					}
					
					subscription->OnResponseReceived(rtsp_message);
				}
				else if(rtsp_message->GetMessageType() == RtspMessageType::REQUEST)
				{
					//TODO(Getroot): What kind of request message will be received?
					logti("%s", rtsp_message->DumpHeader().CStr());
				}
				else
				{
					// Error
					Stop();
					logte("%s/%s(%u) - Unknown rtsp message received", GetApplicationInfo().GetName().CStr(), GetName().CStr(), GetId());
					_state = State::ERROR;
					return ProcessMediaResult::PROCESS_MEDIA_FAILURE;
				}
			}
			else if(_rtsp_demuxer.IsAvaliableData())
			{
				// In an interleaved session, the server sends both messages and data in the same session.
				// Check if there are available messages and interleaved data

				// RTP or RTCP
				auto rtsp_data = _rtsp_demuxer.PopData();
				_rtp_rtcp->OnDataReceived(NodeType::Edge, rtsp_data->GetData());
			}
			else
			{
				return ProcessMediaResult::PROCESS_MEDIA_TRY_AGAIN;
			}
		}

		return ProcessMediaResult::PROCESS_MEDIA_SUCCESS;
	}

	// From RtpRtcp node
	void RtspcStream::OnRtpFrameReceived(const std::vector<std::shared_ptr<RtpPacket>> &rtp_packets)
	{
		auto first_rtp_packet = rtp_packets.front();
		auto payload_type = first_rtp_packet->PayloadType();
		logtd("%s", first_rtp_packet->Dump().CStr());

		auto track = GetTrack(payload_type);
		if(track == nullptr)
		{
			logte("%s - Could not find track : payload_type(%d)", GetName().CStr(), payload_type);
			return;
		}

		auto depacketizer = GetDepacketizer(payload_type);
		if(depacketizer == nullptr)
		{
			logte("%s - Could not find depacketizer : payload_type(%d)", GetName().CStr(), payload_type);
			return;
		}

		std::vector<std::shared_ptr<ov::Data>> payload_list;
		for(const auto &packet : rtp_packets)
		{
			auto payload = std::make_shared<ov::Data>(packet->Payload(), packet->PayloadSize());
			payload_list.push_back(payload);
		}
		
		auto bitstream = depacketizer->ParseAndAssembleFrame(payload_list);
		if(bitstream == nullptr)
		{
			logte("%s - Could not depacketize packet : payload_type(%d)", GetName().CStr(), payload_type);
			return;
		}

		cmn::BitstreamFormat bitstream_format;
		cmn::PacketType packet_type;

		switch(track->GetCodecId())
		{
			case cmn::MediaCodecId::H264:
				// Our H264 depacketizer always converts packet to Annex B
				bitstream_format = cmn::BitstreamFormat::H264_ANNEXB;
				packet_type = cmn::PacketType::NALU;
				break;
			
			case cmn::MediaCodecId::Opus:
				bitstream_format = cmn::BitstreamFormat::OPUS;
				packet_type = cmn::PacketType::RAW;
				break;

			case cmn::MediaCodecId::Vp8:
				bitstream_format = cmn::BitstreamFormat::VP8;
				packet_type = cmn::PacketType::RAW;
				break;

			// It can't be reached here because it has already failed in GetDepacketizer.
			default:
				return;
		}

		auto timestamp = AdjustTimestamp(first_rtp_packet->PayloadType(), first_rtp_packet->Timestamp());

		logtd("Payload Type(%d) Timestamp(%u) Timestamp Delta(%u) Time scale(%f) Adjust Timestamp(%f)", 
				first_rtp_packet->PayloadType(), first_rtp_packet->Timestamp(), timestamp, track->GetTimeBase().GetExpr(), static_cast<double>(timestamp) * track->GetTimeBase().GetExpr());

		auto frame = std::make_shared<MediaPacket>(track->GetMediaType(),
											  track->GetId(),
											  bitstream,
											  timestamp,
											  timestamp,
											  bitstream_format,
											  packet_type);

		logtd("Send Frame : track_id(%d) codec_id(%d) bitstream_format(%d) packet_type(%d) data_length(%d) pts(%u)",  track->GetId(),  track->GetCodecId(), bitstream_format, packet_type, bitstream->GetLength(), first_rtp_packet->Timestamp());
		
		SendFrame(frame);
	}

	// From RtpRtcp node
	void RtspcStream::OnRtcpReceived(const std::shared_ptr<RtcpInfo> &rtcp_info)
	{
		// Nothing to do now
	}

	uint64_t RtspcStream::AdjustTimestamp(uint8_t payload_type, uint32_t timestamp)
	{
		uint32_t curr_timestamp; 

		if(_timestamp_map.find(payload_type) == _timestamp_map.end())
		{
			curr_timestamp = 0;
		}
		else
		{
			curr_timestamp = _timestamp_map[payload_type];
		}

		curr_timestamp += GetTimestampDelta(payload_type, timestamp);

		_timestamp_map[payload_type] = curr_timestamp;

		return curr_timestamp;
	}

	uint64_t RtspcStream::GetTimestampDelta(uint8_t payload_type, uint32_t timestamp)
	{
		// First timestamp
		if(_last_timestamp_map.find(payload_type) == _last_timestamp_map.end())
		{
			_last_timestamp_map[payload_type] = timestamp;
			// Start with zero
			return 0;
		}

		auto delta = timestamp - _last_timestamp_map[payload_type];
		_last_timestamp_map[payload_type] = timestamp;

		return delta;
	}

	ov::String RtspcStream::GenerateControlUrl(ov::String control)
	{
		ov::String prefix = "rtsp://";

		// If contorl is absolute URL, the use it as it is
		if(control.Left(prefix.GetLength()).UpperCaseString() == prefix.UpperCaseString())
		{
			return control;
		}

		// Check content_base
		if(_content_base.IsEmpty() == false)
		{
			return ov::String::FormatString("%s/%s", _content_base.CStr(), control.CStr());
		}

		ov::String control_url;
		control_url = ov::String::FormatString("%s/%s", _curr_url->ToUrlString(false).CStr(), control.CStr());
		if(_curr_url->HasQueryString())
		{
			control_url.AppendFormat("?%s", _curr_url->Query().CStr());
		}

		return control_url;
	}
}

