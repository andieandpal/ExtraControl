// ExtraControl - Aruba Cloud Computing ExtraControl
// Copyright (C) 2012 Aruba S.p.A.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "third-party/base64/base64.h"
#include "serialwatcher.hpp"
#include "packet.hpp"
#include "logging.hpp"
#include "module.hpp"
#include "tools.hpp"
#include "utils/local_buffer.hpp"
#include "third-party/tinyxml/tinyxml.h"

#include <sstream>
#include <cstddef>
#include <utility>
#include <vector>
#include <memory>
#include <algorithm>
#include <fstream>
#include <cstdio>
#include <ctime>

namespace {

std::string restartFileName()
{
	return "serclient.restart";
}

// Return the GUID for a restart packet, if there's one;
// an *empty* string otherwise
//
std::string restartGuid(bool remove = true)
{
    const std::string & name = tools::getRootDirectory() + "/" + restartFileName();
	std::fstream f(name.c_str());
	if (!f.fail()) {
		std::string guid;
		std::getline(f, guid);
		if (!f.fail()) {
			return guid;
		}
	}

	if (remove) {
		std::remove(name.c_str());
	}
	return "";
}

}


bool
hasTelnetIAC(const Packet & packet)
{
	std::vector<char> raw = packet.toVector();
	return std::find(raw.begin(), raw.end(), static_cast<char>(-1))
	       != raw.end();
}

const int SerialWatcher::SERIAL_MIN_READ = 100000;
const int SerialWatcher::LOGIC_TIMEOUT = 30;

SerialWatcher::SerialWatcher(const std::string & port, int command_timeout)
	: _port_name(port),
	  _serial_port(),
	  _process_command_queue(true),
	  _command_timeout(command_timeout)
{
}

void SerialWatcher::start(int baud_rate, int byte_size, int parity, int stop_bits, bool& stopflag)
{
	const long timeout_in_ms = 1000;

	logger->info() << "Trying to open serial port '" << _port_name << "'";

	try {
		_serial_port.reset(new serial::Serial(_port_name, baud_rate,
		                                      serial::Timeout::simpleTimeout(timeout_in_ms),
		                                      static_cast<serial::bytesize_t>(byte_size),
		                                      static_cast<serial::parity_t>(parity),
		                                      static_cast<serial::stopbits_t>(stop_bits)));
	} catch (...) {
		logger->emerg() << "Open failed";
		throw;
	}

	logger->info() << "Serial port open successfully";
	logger->info() << "Service version: " << tools::getServiceVersion();


	// Recovering from a restart?
	const std::string g = ::restartGuid();
	if (!g.empty()) {
		logger->info() << "Sending restart/updateSoftware response";
		std::string t;// TODO: t = getUpdateSoftwareLog
		const Packet p = *Packet::createWithAuthResponse(g);
		_thread_map[p.guid()] = new CommandObserver(Packet::createWithResponse(p.guid(), Packet::Success, "", t));
		send(p);
	}


	while (!stopflag) {

        const std::auto_ptr<Packet> p = doRead(1); // timeout = 1 sec
		if (p.get() != NULL) {
			logger->debug() << "Valid packet received";
			reactToPacket(*p);
		}

        // Now process all AuthResponse commands generated by running threads
		{
			tthread::lock_guard<tthread::mutex>(_output_queue_mutex);
			while (!_output_queue.empty()) {
				send(_output_queue.front());
				_output_queue.pop();
			}
		}

		bool done = isIdle();
		if (done && !_process_command_queue) {
			_process_command_queue = true;
			logger->debug() << "Leaving blocking mode";
		}

		// If enabled, process queued commands
		if (_process_command_queue) {
			while (!_command_queue.empty()) {
				if (_command_queue.front().isBlocking()) {
					_process_command_queue = false;
					if (!done) {
						logger->debug() << "Entering blocking mode";
					} else {
						logger->debug() << "Spawning blocking command";
						CommandRequest req = _command_queue.front();
						_command_queue.pop();
						spawnCommand(req);
					}
					break;
				} else {
					CommandRequest req = _command_queue.front();
					_command_queue.pop();
					spawnCommand(req);
					done = false;
				}
			}
		} else {
			logger->debug() << "Waiting for non-blocking processes to terminate: " << _thread_map.size();
		}
	}
}

void SerialWatcher::send(const Packet & packet)
{
	logger->info() << "Sending packet: " << packet;

	if (hasTelnetIAC(packet)) {
		logger->debug() << "IAC FOUND";
	}

	const std::vector<char> v = packet.toVector();
	const std::string raw(v.begin(), v.end()); // TODO: improve
	logger->debug() << "Buffer size: " << raw.size() << " - " << raw;
	const std::size_t chunk_size = 8 * 1024;
	std::string remaining = raw;
	std::size_t written_size = 0;

	while (remaining.length() != 0) {
		const std::string chunk = remaining.substr(0, chunk_size);
		if (_serial_port->write(chunk/*.c_str(), chunk.length()) != failure*/
		   ) == chunk.length()) { // TODO: check --timeout??
			written_size += chunk.length();
			logger->debug() << "Written to serial port: " << written_size <<
				"/" << raw.length() << " bytes";
			remaining = remaining.size() >= chunk_size
				? remaining.substr(chunk_size, chunk_size)
				: "";
		} else {
			logger->warn() << "Could not write to serial port.";
		}
	}
}

void SerialWatcher::sendLater(const Packet & packet)
{
	tthread::lock_guard<tthread::mutex> guard(_output_queue_mutex);
	_output_queue.push(packet);
}

std::auto_ptr<Packet> SerialWatcher::doRead(int timeout_sec)
{
    std::auto_ptr<Packet> last_packet(NULL);
    time_t started = time(NULL);
    time_t logic_timeout = (time_t) -1;

    while (started != ((time_t) -1)) // If time(NULL) failed skips reading
    {
        if (timeout_sec != 0 && time(NULL)-started > timeout_sec)
            break;
        if ( Packet::hasHeader(_buffer) ) // Buffer is big enough to hold a packet
        {
            try // Check for a valid header
            {
				logger->debug() << "Reading header...";
                Packet::header h = Packet::extractHeader(_buffer); // raises an std::runtime_error on malformed or incomplete headers

                if (logic_timeout == (time_t)-1) //
                {
                    logic_timeout = time(NULL);
                }
                else if (time(NULL) - logic_timeout > LOGIC_TIMEOUT)
                {
					logger->debug() << "LOGIC TIMEOUT detected, looking for new packet";
                    send(*Packet::createWithReceived(h._guid, h._packet_number, h._packet_count, true));
                    std::vector<char>(_buffer.begin()+1, _buffer.end()).swap(_buffer);
                    logic_timeout = (time_t)-1;
                    continue;
                }
            }
            catch (std::runtime_error& e)
            {
                int s = 1;
                while( s < static_cast<int>(_buffer.size()) && _buffer[s] != 0x02 && s < 5000)
                {
                    ++s;
                }
                std::vector<char>(_buffer.begin()+s, _buffer.end()).swap(_buffer);
				logger->debug() << "Header not found: skipped " << s << " byte from read buffer";
                continue;
            }

            if (Packet::hasHeaderAndFooter(_buffer))
            {
				logger->debug() << "Valid footer received";
                try
                {
                    last_packet.reset(new Packet(_buffer));
					logger->debug() << "Packet received: " << *last_packet;
                    std::vector<char>(_buffer.begin()+last_packet->size(), _buffer.end()).swap(_buffer);
                    if (last_packet->isSinglePacket())
                    {
                        return last_packet;
                    }
                    else
                    {
                        _pool.add(*last_packet);
                        if (_pool.hasAllPacketsFor(*last_packet)) {
                            std::auto_ptr<Packet> p = _pool.getFullPacketForGuid(last_packet->guid());
                            _pool.removeAllPacketsFor(*last_packet);
							logger->debug() << "Multiple packets aggregated (guid=" << p->guid() << ")";
                            return p;
                        } else {
                            const std::auto_ptr<Packet> response = Packet::createWithReceived(last_packet->guid(),
                                                        last_packet->number(), last_packet->count());
                            send(*response);
                        }
                    }

                }
                catch (std::runtime_error& e)
                {
					logger->crit() << "Error decoding packet: " << e.what();
                    std::vector<char>(_buffer.begin()+1, _buffer.end()).swap(_buffer);
                }
            }
            else
            {
				logger->debug() << "Waiting for more bytes";
                std::string v = _serial_port->read(SerialWatcher::SERIAL_MIN_READ);
                if (!v.empty())
                {
                    _buffer.insert(_buffer.end(), v.begin(), v.end());
					logger->debug() << "Reading: buffer size " << _buffer.size() << " - " << std::hex << v;
                }
            }
        }
        else
        {
            logic_timeout = (time_t) -1;
            std::string v = _serial_port->read(SerialWatcher::SERIAL_MIN_READ);
            if (!v.empty())
            {
                _buffer.insert(_buffer.end(), v.begin(), v.end());
				logger->debug() << "Reading: buffer size " << _buffer.size() << " - " << v;
            }
        }
        //if ()
    }
    return last_packet;
}

void SerialWatcher::reactToPacket(const Packet & packet)
{
	logger->info() << Packet::commandTypeToString(packet.command())
	    << " received (guid: " << packet.guid() << ")";
	switch (packet.command()) {
	case Packet::Ack:
		send(*Packet::createWithAck(packet.guid()));
		break;
	case Packet::Command:
		processCommand(packet);
		break;
	case Packet::Received:
		// nothing to do
		break;
	case Packet::AuthResponse:
		processAuthResponse(packet);
		break;
	case Packet::Response:
		// nothing to do
		break;
	}
}

void SerialWatcher::processCommand(const Packet & packet)
{
	assert(packet.command() == Packet::Command);

	// Parse the packet body, looking for CommandString and binaryData
	logger->debug() << "XML: " << packet.body();
	
	TiXmlDocument doc;
	doc.Parse(packet.body().c_str());
	if (doc.Error()) {
		logger->crit() << "Malformed XML: " << doc.ErrorDesc();
		send(*Packet::createWithAuthResponse(packet.guid()));
		_thread_map[packet.guid()] = new CommandObserver(Packet::createWithResponse(packet.guid(), Packet::Error));
	}

	TiXmlNode * root = doc.RootElement();
	if (root == NULL) {
		logger->crit() << "Empty xml document in 'command' Packet";
		send(*Packet::createWithAuthResponse(packet.guid()));
		_thread_map[packet.guid()] = new CommandObserver(Packet::createWithResponse(packet.guid(), Packet::Error));
		return;
	}

	if (std::string(root->Value()) != "command") {
		logger->crit() << "Malformed command XML received: expected tag 'command', received '"
		               << root->Value() << "'";
		
		send(*Packet::createWithAuthResponse(packet.guid()));
		_thread_map[packet.guid()] = new CommandObserver(Packet::createWithResponse(packet.guid(), Packet::Error));
		return;
	}

	if (root->FirstChild("commandString") == NULL) {
		logger->crit() << "Malformed command XML received: no 'commandString' found";
		send(*Packet::createWithAuthResponse(packet.guid()));
		_thread_map[packet.guid()] = new CommandObserver(Packet::createWithResponse(packet.guid(), Packet::Error));
		return;
	}

    std::string binary_data_fn = "";

	TiXmlNode * element = root->FirstChild("binaryData");
	if (element != NULL && !element->ValueTStr().empty())
	{
		std::string tmp = base64_decode(element->ValueTStr());

        char tmp_filename[256];
		tmpnam(tmp_filename);

        size_t sz = tmp.size();
        FILE* fd = fopen(tmp_filename, "wb+");

        if (sz == 0 ||
            fd == NULL ||
            fwrite(tmp.c_str(), sizeof(char), tmp.size(), fd) != sz ||
            fclose(fd) != 0)
		{
			logger->crit() << "Malformed or empty base64encoded binary data";
			send(*Packet::createWithAuthResponse(packet.guid()));
			_thread_map[packet.guid()] = new CommandObserver(Packet::createWithResponse(packet.guid(), Packet::Error));
			return;
		}
        binary_data_fn.assign(tmp_filename);
	}

	element = root->FirstChild("commandString");
	std::string command = element->FirstChild() != NULL
		? element->FirstChild()->Value()
		: "";

	send(*Packet::createWithReceived(packet.guid()));
    _command_queue.push(CommandRequest(command, packet.guid(), binary_data_fn));
}

void SerialWatcher::processAuthResponse(const Packet & packet)
{
	assert(packet.command() == Packet::AuthResponse);
	thread_map_type::iterator ite = _thread_map.find(packet.guid());
	if (ite == _thread_map.end() || ite->second == NULL)
	{
		logger->err() << "Response requested for an unknow packet id: " << packet.guid();
		send(*Packet::createWithResponse(packet.guid(),Packet::Error));
	}
	else
	{
		send(*ite->second->responsePacket());
		delete ite->second;
		_thread_map.erase(ite);
	}
}

void SerialWatcher::spawnCommand(const CommandRequest & request)
{
	if (request.command() == "restart" ||
			request.isUpdateSoftware()) {
		tools::saveRestartGUID(request.guid());
	}

	CommandObserver * co = new CommandObserver(request, _command_timeout, *this);
	co->start();
	_thread_map[request.guid()] = co;
}

bool SerialWatcher::isIdle() const
{
	for (thread_map_type::const_iterator it = _thread_map.begin(); it != _thread_map.end(); ++it) {
		if (it->second->isRunning()) {
			return false;
		}
	}
	return true;
}

PacketPool::iterator PacketPool::begin()
{
	return _pool.begin();
}

PacketPool::const_iterator PacketPool::begin() const
{
	return _pool.begin();
}

PacketPool::iterator PacketPool::end()
{
	return _pool.end();
}

PacketPool::const_iterator PacketPool::end() const
{
	return _pool.end();
}


// vim: set ft=cpp noet ts=4 sts=4 sw=4: