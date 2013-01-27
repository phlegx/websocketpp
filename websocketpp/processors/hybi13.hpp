/*
 * Copyright (c) 2013, Peter Thorson. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the WebSocket++ Project nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL PETER THORSON BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 */

#ifndef WEBSOCKETPP_PROCESSOR_HYBI13_HPP
#define WEBSOCKETPP_PROCESSOR_HYBI13_HPP

// For htonl
#if defined(WIN32)
	#include <winsock2.h>
#else
	#include <arpa/inet.h>
#endif

#include <cassert>

#include <websocketpp/frame.hpp>
#include <websocketpp/utf8_validator.hpp>

#include <websocketpp/processors/processor.hpp>

#include <websocketpp/sha1/sha1.hpp>
#include <websocketpp/base64/base64.hpp>

#include <string>
#include <vector>
#include <utility>

namespace websocketpp {
namespace processor {

template <typename config>
class hybi13 : public processor<config> {
public:
    typedef processor<config> base;

	typedef typename config::request_type request_type;
	typedef typename config::response_type response_type;
    
	typedef typename config::message_type message_type;
	typedef typename message_type::ptr message_ptr;

	typedef typename config::con_msg_manager_type msg_manager_type;
	typedef typename msg_manager_type::ptr msg_manager_ptr;

    typedef typename config::permessage_compress_type permessage_compress_type;
    
    typedef std::pair<lib::error_code,std::string> err_str_pair;

    explicit hybi13(bool secure, bool server, msg_manager_ptr manager) 
      : processor<config>(secure,server)
	  , m_msg_manager(manager)
	{
		reset_headers();
	} 
    
    int get_version() const {
        return 13;
    }
    
    bool has_permessage_compress() const {
        return m_permessage_compress.is_implimented();
    }

    err_str_pair negotiate_extensions(const request_type& req) {
        err_str_pair ret;

        // Respect blanket disabling of all extensions and don't even parse
        // the extension header
        if (!config::enable_extensions) {
            ret.first = make_error_code(error::extensions_disabled);
            return ret;
        }
        
        typename request_type::parameter_list p;

        bool error = req.get_header_as_plist("Sec-WebSocket-Extensions",p);
        
        if (error) {
            ret.first = make_error_code(error::extension_parse_error);
            return ret;
        }
        
        // If there are no extensions parsed then we are done!
        if (p.size() == 0) {
            return ret;
        }
        
        typename request_type::parameter_list::const_iterator it;
        
        // if permessage_compress is implimented, check if it was requested
        if (m_permessage_compress.is_implimented()) {
            it = p.find("permessage-compress");
            
            err_str_pair neg_ret;

            if (it != p.end()) {
                neg_ret = m_permessage_compress.negotiate(it->second);

                if (neg_ret.first) {
                    // Figure out if this is an error that should halt all
                    // extension negotiations or simply cause negotiation of
                    // this specific extension to fail.
                    std::cout << "permessage-compress negotiation failed: " 
                              << neg_ret.first << " and message " 
                              << neg_ret.second << std::endl;
                } else {
                    // Note: this list will need commas if WebSocket++ ever
                    // supports more than one extension
                    ret.second += neg_ret.second;
                }
            }
        }

        return ret;
    }

    lib::error_code validate_handshake(const request_type& r) const {
        if (r.get_method() != "GET") {
            return make_error_code(error::invalid_http_method);
        }
        
        if (r.get_version() != "HTTP/1.1") {
            return make_error_code(error::invalid_http_version);
        }
        
        // required headers
        // Host is required by HTTP/1.1
        // Connection is required by is_websocket_handshake
        // Upgrade is required by is_websocket_handshake
        if (r.get_header("Sec-WebSocket-Key") == "") {
            return make_error_code(error::missing_required_header);
        }
        
        return lib::error_code();
    }
    
    lib::error_code process_handshake(const request_type& request, 
        response_type& response) const
	{
        std::string server_key = request.get_header("Sec-WebSocket-Key");
        server_key.append(constants::handshake_guid);
                
        SHA1        sha;
        uint32_t    message_digest[5];
        
        sha << server_key.c_str();
                
        if (sha.Result(message_digest)){
            // convert sha1 hash bytes to network byte order because this sha1
            //  library works on ints rather than bytes
            for (int i = 0; i < 5; i++) {
                message_digest[i] = htonl(message_digest[i]);
            }
            
            server_key = base64_encode(
                reinterpret_cast<const unsigned char*>(message_digest),
                20
            );
            
            // set handshake accept headers
            response.replace_header("Sec-WebSocket-Accept",server_key);
            response.append_header("Upgrade",constants::upgrade_token);
            response.append_header("Connection",constants::connection_token);
        } else {
            return make_error_code(error::sha1_library);
        }
        
        return lib::error_code();
    }
    
    std::string get_raw(const response_type& res) const {
        return res.raw();
    }
    
    const std::string& get_origin(const request_type& r) const {
        return r.get_header("Origin");
    }
    
    uri_ptr get_uri(const request_type& request) const {
        std::string h = request.get_header("Host");
        
        size_t last_colon = h.rfind(":");
        size_t last_sbrace = h.rfind("]");
                
        // no : = hostname with no port
        // last : before ] = ipv6 literal with no port
        // : with no ] = hostname with port
        // : after ] = ipv6 literal with port
        if (last_colon == std::string::npos || 
            (last_sbrace != std::string::npos && last_sbrace > last_colon))
        {
            return uri_ptr(new uri(base::m_secure,h,request.get_uri()));
        } else {
            return uri_ptr(new uri(base::m_secure,
                                   h.substr(0,last_colon),
                                   h.substr(last_colon+1),
                                   request.get_uri()));
        }
        
        // TODO: check if get_uri is a full uri
    }

	/// Process new websocket connection bytes
	/**
	 * 
	 * Hybi 13 data streams represent a series of variable length frames. Each 
	 * frame is made up of a series of fixed length fields. The lengths of later
	 * fields are contained in earlier fields. The first field length is fixed
	 * by the spec.
	 *
	 * This processor represents a state machine that keeps track of what field
	 * is presently being read and how many more bytes are needed to complete it
	 * 
	 * 
	 * 
	 * 
	 * Read two header bytes
	 *   Extract full frame length.
	 *   Read extra header bytes
	 * Validate frame header (including extension validate)
	 * Read extension data into extension message state object
	 * Read payload data into payload
	 * 
	 * @param buf Input buffer
	 * 
	 * @param len Length of input buffer
	 * 
	 * @return Number of bytes processed or zero on error
	 */
	size_t consume(uint8_t * buf, size_t len, lib::error_code & ec) {		
		size_t p = 0;
	    
        ec = lib::error_code();

		//std::cout << "consume: " << utility::to_hex(buf,len) << std::endl;

		// Loop while we don't have a message ready and we still have bytes 
		// left to process. 
		while (m_state != READY && m_state != FATAL_ERROR && 
			   (p < len || m_bytes_needed == 0))
		{
			if (m_state == HEADER_BASIC) {
				p += this->copy_basic_header_bytes(buf+p,len-p);
				
				if (m_bytes_needed > 0) {
					continue;	
				}
                
                ec = this->validate_incoming_basic_header(
                    m_basic_header, base::m_server, !m_data_msg.msg_ptr
                );
				if (ec) {break;}

				// extract full header size and adjust consume state accordingly
				m_state = HEADER_EXTENDED;
				m_cursor = 0;
				m_bytes_needed = frame::get_header_len(m_basic_header) - 
					frame::BASIC_HEADER_LENGTH;
			} else if (m_state == HEADER_EXTENDED) {
				p += this->copy_extended_header_bytes(buf+p,len-p);

				if (m_bytes_needed > 0) {
					continue;
				}
				
                ec = validate_incoming_extended_header(m_basic_header,m_extended_header);
				if (ec){break;}
					
				m_state = APPLICATION;
				m_bytes_needed = get_payload_size(m_basic_header,m_extended_header);
					
				// check if this frame is the start of a new message and set up
				// the appropriate message metadata.
				frame::opcode::value op = frame::get_opcode(m_basic_header);

				if (frame::opcode::is_control(op)) {
					m_control_msg = msg_metadata(
						m_msg_manager->get_message(op,m_bytes_needed),
						frame::get_masking_key(m_basic_header,m_extended_header)
					);
					
					m_current_msg = &m_control_msg;
				} else {
					if (!m_data_msg.msg_ptr) {
						m_data_msg = msg_metadata(
							m_msg_manager->get_message(op,m_bytes_needed),
							frame::get_masking_key(m_basic_header,m_extended_header)
						);
					} else {
                        // Each frame starts a new masking key. All other state
                        // remains between frames.
                        m_data_msg.prepared_key = prepare_masking_key(
                            frame::get_masking_key(
                                m_basic_header,
                                m_extended_header
                            )
                        );
						// TODO: reserve space in the existing message for the new bytes
					}
					m_current_msg = &m_data_msg;
				}
			} else if (m_state == EXTENSION) {
				m_state = APPLICATION;
			} else if (m_state == APPLICATION) {
				size_t bytes_to_process = std::min(m_bytes_needed,len-p);
				
				if (bytes_to_process > 0) {
					p += this->process_payload_bytes(buf+p,bytes_to_process,ec);
					
					if (ec) {break;}
				}

				if (m_bytes_needed > 0) {
					continue;
				}
				
				// If this was the last frame in the message set the ready flag.
				// Otherwise, reset processor state to read additional frames.
				if (frame::get_fin(m_basic_header)) {
					// ensure that text messages end on a valid UTF8 code point
					if (frame::get_opcode(m_basic_header) == frame::opcode::TEXT) {
						if (!m_current_msg->validator.complete()) {
                            ec = make_error_code(error::invalid_utf8);
							break;
						}
					}

					m_state = READY;
				} else {
					this->reset_headers();
				}
			} else {
				// shouldn't be here
                ec = make_error_code(error::generic);
				return 0;
			}
		}
		
		return p;
	}
	
	void reset_headers() {
		m_state = HEADER_BASIC;
		m_bytes_needed = frame::BASIC_HEADER_LENGTH;

		m_basic_header.b0 = 0x00;
		m_basic_header.b1 = 0x00;

		std::fill_n(
			m_extended_header.bytes,
			frame::MAX_EXTENDED_HEADER_LENGTH,
			0x00
		);
	}
	
	/// Test whether or not the processor has a message ready
	bool ready() const {
		return (m_state == READY);
	}
	
	message_ptr get_message() {
		if (!ready()) {
			return message_ptr();
		}
		message_ptr ret = m_current_msg->msg_ptr;
		m_current_msg->msg_ptr.reset();

		if (frame::opcode::is_control(ret->get_opcode())) {
			m_control_msg.msg_ptr.reset();
		} else {
			m_data_msg.msg_ptr.reset();
		}
		
		this->reset_headers();
		
		return ret;
	}
	
	/// Test whether or not the processor is in a fatal error state.
	bool get_error() const {
		return m_state == FATAL_ERROR;
	}

	size_t get_bytes_needed() const {
		return m_bytes_needed;
	}

	/// Prepare a user data message for writing
	/**
	 * Performs validation, masking, compression, etc. will return an error if 
	 * there was an error, otherwise msg will be ready to be written
	 *
	 * By default WebSocket++ performs block masking/unmasking in a manner that 
	 * makes assumptions about the nature of the machine and STL library used. 
	 * In particular the assumption is either a 32 or 64 bit word size and an 
	 * STL with std::string::data returning a contiguous char array.
	 * 
	 * This method improves masking performance by 3-8x depending on the ratio 
	 * of small to large messages and the availability of a 64 bit processor.
	 * 
	 * To disable this optimization (for use with alternative STL 
	 * implementations or processors) define WEBSOCKETPP_STRICT_MASKING when
	 * compiling the library. This will force the library to perform masking in
	 * single byte chunks.
	 * 
	 * TODO: tests
	 * 
	 * @param in An unprepared message to prepare
	 *
	 * @param out A message to be overwritten with the prepared message
	 *
	 * @return error code
	 */
	virtual lib::error_code prepare_data_frame(message_ptr in, message_ptr out)
	{
		if (!in || !out) {
			return make_error_code(error::invalid_arguments);
		}
		
        frame::opcode::value op = in->get_opcode();

        // validate opcode: only regular data frames 
        if (frame::opcode::is_control(op)) {
			return make_error_code(error::invalid_opcode);
		}
		
		std::string& i = in->get_raw_payload();
		std::string& o = out->get_raw_payload();
		
		// validate payload utf8
		if (op == frame::opcode::TEXT && !utf8_validator::validate(i)) {
			return make_error_code(error::invalid_payload);
		}
		
		frame::masking_key_type key;
		bool masked = !base::m_server;
        bool compressed = m_permessage_compress.is_enabled() 
                          && in->get_compressed();
        bool fin = in->get_fin();
		
		// generate header
		frame::basic_header h(op,i.size(),fin,masked,compressed);
		
		if (masked) {
			// Generate masking key.
			// TODO: figure out where the RNG should live
			// TODO: actually use a RNG here
			key.i = 0;
			
			frame::extended_header e(i.size(),key.i);
			out->set_header(frame::prepare_header(h,e));
		} else {
			frame::extended_header e(i.size());
			out->set_header(frame::prepare_header(h,e));
		}
				
		// prepare payload
		if (compressed) {
			// compress and store in o after header.
            m_permessage_compress.compress(i,o);

			// mask in place if necessary
            if (masked) {
                this->masked_copy(o,o,key);
            }
		} else {
			// no compression, just copy data into the output buffer
			o.resize(i.size());
			
			// if we are masked, have the masking function write to the output
			// buffer directly to avoid another copy. If not masked, copy
			// directly without masking.
			if (masked) {
                this->masked_copy(i,o,key);
			} else {
				std::copy(i.begin(),i.end(),o.begin());
			}
		}

		out->set_prepared(true);
		
		return lib::error_code();
	}

    lib::error_code prepare_ping(const std::string& in, message_ptr out) const {
        return this->prepare_control(frame::opcode::PING,in,out);
    }

    lib::error_code prepare_pong(const std::string& in, message_ptr out) const {
        return this->prepare_control(frame::opcode::PONG,in,out);
    }

    virtual lib::error_code prepare_close(close::status::value code, 
        const std::string & reason, message_ptr out) const
    {
        if (close::status::reserved(code)) {
            return make_error_code(error::reserved_close_code);
        }

        if (close::status::invalid(code) && code != close::status::no_status) {
            return make_error_code(error::invalid_close_code);
        }
        
        if (code == close::status::no_status && reason.size() > 0) {
            return make_error_code(error::reason_requires_code);
        }

        if (reason.size() > frame:: limits::payload_size_basic-2) {
            return make_error_code(error::control_too_big);
        }
        
        std::string payload;
        
        if (code != close::status::no_status) {
            close::code_converter val;
            val.i = htons(code);

            payload.resize(reason.size()+2);
            
            payload[0] = val.c[0];
            payload[1] = val.c[1];

            std::copy(reason.begin(),reason.end(),payload.begin()+2);
        }
                
        return this->prepare_control(frame::opcode::CLOSE,payload,out);
    }
protected:
	/// Reads bytes from buf into m_basic_header
	size_t copy_basic_header_bytes(const uint8_t * buf, size_t len) {
		if (len == 0 || m_bytes_needed == 0) {
			return 0;
		}
		
		if (len > 1) {
			// have at least two bytes
			if (m_bytes_needed == 2) {
				m_basic_header.b0 = buf[0];
				m_basic_header.b1 = buf[1];
				m_bytes_needed -= 2;
				return 2;
			} else {
				m_basic_header.b1 = buf[0];
				m_bytes_needed--;
				return 1;
			}
		} else {
			// have exactly one byte
			if (m_bytes_needed == 2) {
				m_basic_header.b0 = buf[0];
				m_bytes_needed--;
				return 1;
			} else {
				m_basic_header.b1 = buf[0];
				m_bytes_needed--;
				return 1;
			}
		}
	}

	/// Reads bytes from buf into m_extended_header
	size_t copy_extended_header_bytes(const uint8_t * buf, size_t len) {
		size_t bytes_to_read = std::min(m_bytes_needed,len);
		
		std::copy(buf,buf+bytes_to_read,m_extended_header.bytes+m_cursor);
		m_cursor += bytes_to_read;
		m_bytes_needed -= bytes_to_read;

		return bytes_to_read;
	}

	/// Reads bytes from buf into message payload
	/**
     * This function performs unmasking and uncompression, validates the 
	 * decoded bytes, and writes them to the appropriate message buffer.
	 * 
	 * This member function will use the input buffer as stratch space for its 
	 * work. The raw input bytes will not be preserved. This applies only to the
	 * bytes actually needed. At most min(m_bytes_needed,len) will be processed.
	 * 
	 * @param buf Input/working buffer
	 *
	 * @param len Length of buf
	 *
	 * @return Number of bytes processed or zero in case of an error
	 * 
	 */
	size_t process_payload_bytes(uint8_t * buf, size_t len, lib::error_code& ec)
    {
		// unmask if masked
		if (frame::get_masked(m_basic_header)) {
			m_current_msg->prepared_key = frame::word_mask_circ(
				buf,
				len,
				m_current_msg->prepared_key
			);
		}
        
        std::string& out = m_current_msg->msg_ptr->get_raw_payload();
        size_t offset = out.size();

		// decompress message if needed.
		if (m_permessage_compress.is_enabled() 
            && frame::get_rsv1(m_basic_header))
        {
            // Decompress current buffer into the message buffer
            m_permessage_compress.decompress(buf,len,out);
            
            // get the length of the newly uncompressed output
            offset = out.size() - offset;
        } else {
            // No compression, straight copy
            out.append(reinterpret_cast<char *>(buf),len);
        }

		// validate unmasked, decompressed values
		if (m_current_msg->msg_ptr->get_opcode() == frame::opcode::TEXT) {
			if (!m_current_msg->validator.decode(out.begin()+offset,out.end())) {
                ec = make_error_code(error::invalid_utf8);
				return 0;
			}
		}
	
		// copy into message buffer
		/*m_current_msg->msg_ptr->append_payload(
			reinterpret_cast<char *>(buf),
			len
		);*/
		m_bytes_needed -= len;

		return len;
	}
	
	/// Validate an incoming basic header
	/**
	 * Validates an incoming hybi13 basic header.
	 * 
     * @param h The basic header to validate
     *
     * @param is_server Whether or not the endpoint that received this frame
     * is a server.
     *
     * @param new_msg Whether or not this is the first frame of the message
     *
	 * @return 0 on success or a non-zero error code on failure
	 */
    lib::error_code validate_incoming_basic_header(const frame::basic_header &h, 
        bool is_server, bool new_msg) const
    {
        frame::opcode::value op = frame::get_opcode(h);

		// Check control frame size limit
		if (frame::opcode::is_control(op) && 
            frame::get_basic_size(h) > frame::limits::payload_size_basic)
		{
            return make_error_code(error::control_too_big);
		}

		// Check that RSV bits are clear
        // The only RSV bits allowed are rsv1 if the permessage_compress
        // extension is enabled for this connection and the message is not
        // a control message.
        //
        // TODO: unit tests for this
        if (frame::get_rsv1(h) && (!m_permessage_compress.is_enabled() 
                || frame::opcode::is_control(op)))
        {        
            return make_error_code(error::invalid_rsv_bit);
        }

        if (frame::get_rsv2(h) || frame::get_rsv3(h)) {
            return make_error_code(error::invalid_rsv_bit);
		}

		// Check for reserved opcodes
		if (frame::opcode::reserved(op)) {
            return make_error_code(error::invalid_opcode);
		}

		// Check for invalid opcodes
		// TODO: unit tests for this?
		if (frame::opcode::invalid(op)) {
			return make_error_code(error::invalid_opcode);
		}

		// Check for fragmented control message
		if (frame::opcode::is_control(op) && !frame::get_fin(h)) {
            return make_error_code(error::fragmented_control);
		}
		
		// Check for continuation without an active message
		if (new_msg && op == frame::opcode::CONTINUATION) {
            return make_error_code(error::invalid_continuation);
		}
		
        // Check for new data frame when expecting continuation
        if (!new_msg && !frame::opcode::is_control(op) &&
            op != frame::opcode::CONTINUATION)
        {
            return make_error_code(error::invalid_continuation);
        }

		// Servers should reject any unmasked frames from clients.
		// Clients should reject any masked frames from servers.
		if (is_server && !frame::get_masked(h)) {
            return make_error_code(error::masking_required);
		} else if (!is_server && frame::get_masked(h)) {
            return make_error_code(error::masking_forbidden);
		}

		return lib::error_code();
	}
	
	/// Validate an incoming extended header
	/**
     * Validates an incoming hybi13 full header.
     *
     * @param h The basic header to validate
     *
     * @param e The extended header to validate
     *
     * @return An error_code, non-zero values indicate why the validation 
     * failed
     */
    lib::error_code validate_incoming_extended_header(const frame::basic_header h, 
        const frame::extended_header e) const
    {
		uint8_t basic_size = frame::get_basic_size(h);
		uint64_t payload_size = frame::get_payload_size(h,e);
		
		// Check for non-minimally encoded payloads
		if (basic_size == frame::payload_size_code_16bit && 
			payload_size <= frame::limits::payload_size_basic)
		{
            return make_error_code(error::non_minimal_encoding);
		}

		if (basic_size == frame::payload_size_code_64bit && 
			payload_size <= frame::limits::payload_size_extended)
		{
            return make_error_code(error::non_minimal_encoding);
		}

		// Check for >32bit frames on 32 bit systems
		// TODO: unit test for this case
		if (sizeof(size_t) == 4 && (payload_size >> 32)) {
            return make_error_code(error::requires_64bit);
		}

		return lib::error_code();
	}
	
    void masked_copy (const std::string & i, std::string & o,
        frame::masking_key_type key) const
    {
        #ifdef WEBSOCKETPP_STRICT_MASKING
            frame::byte_mask(i.begin(),i.end(),o.begin(),key);
        #else
            websocketpp::frame::word_mask_exact(
                reinterpret_cast<uint8_t*>(const_cast<char*>(i.data())),
                reinterpret_cast<uint8_t*>(const_cast<char*>(o.data())),
                i.size(),
                key
            );
        #endif
    }

    /// Generic prepare control frame with opcode and payload.
    /**
     * Internal control frame building method. Handles validation, masking, etc
     *
     * @param op The control opcode to use
     *
     * @param payload The payload to use
     *
     * @param out The message buffer to store the prepared frame in
     *
     * @return Status code, zero on success, non-zero on error
     */
    lib::error_code prepare_control(frame::opcode::value op, 
        const std::string & payload, message_ptr out) const
    {
		if (!out) {
			return make_error_code(error::invalid_arguments);
		}

        if (!frame::opcode::is_control(op)) {
            return make_error_code(error::invalid_opcode);
        }

		if (payload.size() > frame::limits::payload_size_basic) {
			return make_error_code(error::control_too_big);
		}

		frame::masking_key_type key;
		bool masked = !base::m_server;
		
		frame::basic_header h(op,payload.size(),true,masked);

		std::string& o = out->get_raw_payload();
        o.resize(payload.size());

		if (masked) {
			// Generate masking key.
			// TODO: figure out where the RNG should live
			// TODO: actually use a RNG here
			key.i = 0;
			
			frame::extended_header e(payload.size(),key.i);
			out->set_header(frame::prepare_header(h,e));
            this->masked_copy(payload,o,key);
		} else {
			frame::extended_header e(payload.size());
			out->set_header(frame::prepare_header(h,e));
            //std::cout << "o: " << o.size() << std::endl; 
            std::copy(payload.begin(),payload.end(),o.begin());
            //std::cout << "o: " << o.size() << std::endl; 
		}
				
		out->set_prepared(true);
		
		return lib::error_code();
    }

	enum state {
		HEADER_BASIC = 0,
		HEADER_EXTENDED = 1,
		EXTENSION = 2,
		APPLICATION = 3,
		READY = 4,
		FATAL_ERROR = 5
	};
	
	/// This data structure holds data related to processing a message, such as 
	/// the buffer it is being written to, its masking key, its UTF8 validation 
	/// state, and sometimes its compression state.
	struct msg_metadata {
		msg_metadata() {}
		msg_metadata(message_ptr m, size_t p) : msg_ptr(m),prepared_key(p) {}
		msg_metadata(message_ptr m, frame::masking_key_type p) 
		  : msg_ptr(m)
		  , prepared_key(prepare_masking_key(p)) {}

		message_ptr	msg_ptr;        // pointer to the message data buffer
		size_t      prepared_key;   // prepared masking key
		utf8_validator::validator validator; // utf8 validation state
	};
    
    // Basic header of the frame being read
	frame::basic_header m_basic_header;
    
	// Pointer to a manager that can create message buffers for us.
	msg_manager_ptr m_msg_manager;

	// Number of bytes needed to complete the current operation
	size_t m_bytes_needed;
	
	// Number of extended header bytes read
	size_t m_cursor;
	
	// Metadata for the current data msg
	msg_metadata m_data_msg;
	// Metadata for the current control msg
	msg_metadata m_control_msg;

    // Pointer to the metadata associated with the frame being read
	msg_metadata *m_current_msg;
	
	// Extended header of current frame
	frame::extended_header m_extended_header;
    
    
    // write/prep state
    // original opcode
    // utf8 validator
    // compression state



	// Overall state of the processor
	state m_state;

    // Extensions
    permessage_compress_type m_permessage_compress;
};

} // namespace processor
} // namespace websocketpp

#endif //WEBSOCKETPP_PROCESSOR_HYBI13_HPP