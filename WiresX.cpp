/*
*   Copyright (C) 2016,2017 by Jonathan Naylor G4KLX
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "WiresX.h"
#include "YSFPayload.h"
#include "YSFFICH.h"
#include "Utils.h"
#include "Sync.h"
#include "CRC.h"
#include "Log.h"

#include <cstdio>
#include <cassert>
#include <cstdlib>

const unsigned char DX_REQ[]    = {0x5DU, 0x71U, 0x5FU};
const unsigned char CONN_REQ[]  = {0x5DU, 0x23U, 0x5FU};
const unsigned char DISC_REQ[]  = {0x5DU, 0x2AU, 0x5FU};
const unsigned char ALL_REQ[]   = {0x5DU, 0x66U, 0x5FU};

const unsigned char DX_RESP[]   = {0x5DU, 0x51U, 0x5FU, 0x26U};
const unsigned char CONN_RESP[] = {0x5DU, 0x41U, 0x5FU, 0x26U};
const unsigned char DISC_RESP[] = {0x5DU, 0x41U, 0x5FU, 0x26U};
const unsigned char ALL_RESP[]  = {0x5DU, 0x46U, 0x5FU, 0x26U};

const unsigned char DEFAULT_FICH[] = {0x20U, 0x00U, 0x01U, 0x00U};

const unsigned char NET_HEADER[] = "YSFD                    ALL      ";

CWiresX::CWiresX(const std::string& callsign, const std::string& suffix, CYSFNetwork* network) :
m_callsign(callsign),
m_node(),
m_id(),
m_name(),
m_txFrequency(0U),
m_rxFrequency(0U),
m_reflector(0U),
m_network(network),
m_command(NULL),
m_timer(1000U, 1U),
m_seqNo(0U),
m_header(NULL),
m_csd1(NULL),
m_csd2(NULL),
m_csd3(NULL),
m_status(WXSI_NONE),
m_start(0U),
m_search()
{
	assert(network != NULL);

	m_node = callsign;
	if (suffix.size() > 0U) {
		m_node.append("-");
		m_node.append(suffix);
	}
	m_node.resize(YSF_CALLSIGN_LENGTH, ' ');

	m_callsign.resize(YSF_CALLSIGN_LENGTH, ' ');

	m_command = new unsigned char[300U];

	m_header = new unsigned char[34U];
	m_csd1   = new unsigned char[20U];
	m_csd2   = new unsigned char[20U];
	m_csd3   = new unsigned char[20U];
}

CWiresX::~CWiresX()
{
	delete[] m_csd3;
	delete[] m_csd2;
	delete[] m_csd1;
	delete[] m_header;
	delete[] m_command;
}

void CWiresX::setInfo(const std::string& name, unsigned int txFrequency, unsigned int rxFrequency, int reflector)
{
	assert(txFrequency > 0U);
	assert(rxFrequency > 0U);

	m_name        = name;
	m_txFrequency = txFrequency;
	m_rxFrequency = rxFrequency;
	m_reflector = reflector;

	m_name.resize(14U, ' ');

	unsigned int hash = 0U;

	for (unsigned int i = 0U; i < name.size(); i++) {
		hash += name.at(i);
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	// Final avalanche
	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	char id[10U];
	::sprintf(id, "%05u", hash % 100000U);

	LogInfo("The ID of this repeater is %s", id);

	m_id = std::string(id);

	::memset(m_csd1, '*', 20U);
	::memset(m_csd2, ' ', 20U);
	::memset(m_csd3, ' ', 20U);

	for (unsigned int i = 0U; i < 10U; i++)
		m_csd1[i + 10U] = m_node.at(i);

	for (unsigned int i = 0U; i < 10U; i++)
		m_csd2[i + 0U] = m_callsign.at(i);

	for (unsigned int i = 0U; i < 5U; i++) {
		m_csd3[i + 0U]  = m_id.at(i);
		m_csd3[i + 15U] = m_id.at(i);
	}

	for (unsigned int i = 0U; i < 34U; i++)
		m_header[i] = NET_HEADER[i];

	for (unsigned int i = 0U; i < 10U; i++)
		m_header[i + 4U] = m_callsign.at(i);

	for (unsigned int i = 0U; i < 10U; i++)
		m_header[i + 14U] = m_node.at(i);
}


WX_STATUS CWiresX::process(const unsigned char* data, const unsigned char* source, unsigned char fi, unsigned char dt, unsigned char fn, unsigned char ft)
{
	assert(data != NULL);
	assert(source != NULL);

	if (dt != YSF_DT_DATA_FR_MODE)
		return WXS_NONE;

	if (fi != YSF_FI_COMMUNICATIONS)
		return WXS_NONE;

	CYSFPayload payload;

	if (fn == 0U)
		return WXS_NONE;

	if (fn == 1U) {
		bool valid = payload.readDataFRModeData2(data, m_command + 0U);
		if (!valid)
			return WXS_NONE;
	} else {
		bool valid = payload.readDataFRModeData1(data, m_command + (fn - 1U) * 20U + 0U);
		if (!valid)
			return WXS_NONE;

		valid = payload.readDataFRModeData2(data, m_command + (fn - 1U) * 20U + 20U);
		if (!valid)
			return WXS_NONE;
	}

	if (fn == ft) {
		bool valid = false;

		// Find the end marker
		for (unsigned int i = fn * 20U; i > 0U; i--) {
			if (m_command[i] == 0x03U) {
				unsigned char crc = CCRC::addCRC(m_command, i + 1U);
				if (crc == m_command[i + 1U])
					valid = true;
				break;
			}
		}

		if (!valid)
			return WXS_NONE;

		if (::memcmp(m_command + 1U, DX_REQ, 3U) == 0) {
			processDX(source);
			return WXS_DX;
		} else if (::memcmp(m_command + 1U, ALL_REQ, 3U) == 0) {
			processAll(source, m_command + 5U);
			return WXS_ALL;
		} else if (::memcmp(m_command + 1U, CONN_REQ, 3U) == 0) {
			return processConnect(source, m_command + 5U);
		} else if (::memcmp(m_command + 1U, DISC_REQ, 3U) == 0) {
			processDisconnect(source);
			return WXS_DISCONNECT;
		} else {
			CUtils::dump("Unknown Wires-X command", m_command, fn * 20U);
			return WXS_FAIL;
		}
	}

	return WXS_NONE;
}

int CWiresX::getReflector() const
{
	return m_reflector;
}


void CWiresX::processDX(const unsigned char* source)
{
	::LogDebug("Received DX from %10.10s", source);

	m_status = WXSI_DX;
	m_timer.start();
}

void CWiresX::processAll(const unsigned char* source, const unsigned char* data)
{
	if (data[0U] == '0' && data[1] == '1') {
		::LogDebug("Received ALL for \"%3.3s\" from %10.10s", data + 2U, source);

		char buffer[4U];
		::memcpy(buffer, data + 2U, 3U);
		buffer[3U] = 0x00U;

		m_start = ::atoi(buffer);
		if (m_start > 0U)
			m_start--;

		m_status = WXSI_ALL;

		m_timer.start();
	} else if (data[0U] == '1' && data[1U] == '1') {
		::LogDebug("Received SEARCH for \"%16.16s\" from %10.10s", data + 5U, source);

		m_search = std::string((char*)(data + 5U), 16U);

		m_status = WXSI_SEARCH;

		m_timer.start();
	}
}

WX_STATUS CWiresX::processConnect(const unsigned char* source, const unsigned char* data)
{
	::LogDebug("Received Connect to %6.6s from %10.10s", data, source);

	std::string id = std::string((char*)data, 6U);

	m_reflector = atoi(id.c_str());
	if (m_reflector == 0)
		return WXS_NONE;

	m_status = WXSI_CONNECT;
	m_timer.start();

	return WXS_CONNECT;
}

void CWiresX::processConnect(int reflector)
{
	m_reflector = reflector;

	m_status = WXSI_CONNECT;
	m_timer.start();
}

void CWiresX::processDisconnect(const unsigned char* source)
{
	if (source != NULL)
		::LogDebug("Received Disconect from %10.10s", source);

	//m_reflector = 0;

	m_status = WXSI_DISCONNECT;
	m_timer.start();
}

void CWiresX::clock(unsigned int ms)
{
	//m_reflectors.clock(ms);

	m_timer.clock(ms);
	if (m_timer.isRunning() && m_timer.hasExpired()) {
		switch (m_status) {
		case WXSI_DX:
			CThread::sleep(100U);
			sendDXReply();
			break;
		case WXSI_ALL:
			sendAllReply();
			break;
		case WXSI_SEARCH:
			sendSearchReply();
			break;
		case WXSI_CONNECT:
			//CThread::sleep(100U);
			//sendConnectReply();
			break;
		case WXSI_DISCONNECT:
			//sendDisconnectReply();
			break;
		default:
			break;
		}

		m_status = WXSI_NONE;
		m_timer.stop();
	}
}

void CWiresX::createReply(const unsigned char* data, unsigned int length)
{
	assert(data != NULL);
	assert(length > 0U);

	unsigned char bt = 0U;

	if (length > 260U) {
		bt = 1U;
		bt += (length - 260U) / 259U;

		length += bt;
	}

	if (length > 20U) {
		unsigned int blocks = (length - 20U) / 40U;
		if ((length % 40U) > 0U) blocks++;
		length = blocks * 40U + 20U;
	} else {
		length = 20U;
	}

	unsigned char ft = calculateFT(length, 0U);

	unsigned char seqNo = 0U;

	// Write the header
	unsigned char buffer[200U];
	::memcpy(buffer, m_header, 34U);

	CSync::addYSFSync(buffer + 35U);

	CYSFFICH fich;
	fich.load(DEFAULT_FICH);
	fich.setFI(YSF_FI_HEADER);
	fich.setBT(bt);
	fich.setFT(ft);
	fich.encode(buffer + 35U);

	CYSFPayload payload;
	payload.writeDataFRModeData1(m_csd1, buffer + 35U);
	payload.writeDataFRModeData2(m_csd2, buffer + 35U);

	buffer[34U] = seqNo;
	seqNo += 2U;

	m_network->write(buffer);

	fich.setFI(YSF_FI_COMMUNICATIONS);

	unsigned char fn = 0U;
	unsigned char bn = 0U;

	unsigned int offset = 0U;
	while (offset < length) {
		switch (fn) {
		case 0U: {
				ft = calculateFT(length, offset);
				payload.writeDataFRModeData1(m_csd1, buffer + 35U);
				payload.writeDataFRModeData2(m_csd2, buffer + 35U);
			}
			break;
		case 1U:
			payload.writeDataFRModeData1(m_csd3, buffer + 35U);
			if (bn == 0U) {
				payload.writeDataFRModeData2(data + offset, buffer + 35U);
				offset += 20U;
			} else {
				// All subsequent entries start with 0x00U
				unsigned char temp[20U];
				::memcpy(temp + 1U, data + offset, 19U);
				temp[0U] = 0x00U;
				payload.writeDataFRModeData2(temp, buffer + 35U);
				offset += 19U;
			}
			break;
		default:
			payload.writeDataFRModeData1(data + offset, buffer + 35U);
			offset += 20U;
			payload.writeDataFRModeData2(data + offset, buffer + 35U);
			offset += 20U;
			break;
		}

		fich.setFT(ft);
		fich.setFN(fn);
		fich.setBT(bt);
		fich.setBN(bn);
		fich.encode(buffer + 35U);

		buffer[34U] = seqNo;
		seqNo += 2U;

		m_network->write(buffer);

		fn++;
		if (fn >= 8U) {
			fn = 0U;
			bn++;
		}
	}

	// Write the trailer
	fich.setFI(YSF_FI_TERMINATOR);
	fich.setFN(fn);
	fich.setBN(bn);
	fich.encode(buffer + 35U);

	payload.writeDataFRModeData1(m_csd1, buffer + 35U);
	payload.writeDataFRModeData2(m_csd2, buffer + 35U);

	buffer[34U] = seqNo | 0x01U;

	m_network->write(buffer);
}

unsigned char CWiresX::calculateFT(unsigned int length, unsigned int offset) const
{
	length -= offset;

	if (length > 220U) return 7U;

	if (length > 180U) return 6U;

	if (length > 140U) return 5U;

	if (length > 100U) return 4U;

	if (length > 60U)  return 3U;

	if (length > 20U)  return 2U;

	return 1U;
}

void CWiresX::sendDXReply()
{
	unsigned char data[150U];
	::memset(data, 0x00U, 150U);
	::memset(data, ' ', 128U);

	data[0U] = m_seqNo;

	for (unsigned int i = 0U; i < 4U; i++)
		data[i + 1U] = DX_RESP[i];

	for (unsigned int i = 0U; i < 5U; i++)
		data[i + 5U] = m_id.at(i);

	for (unsigned int i = 0U; i < 10U; i++)
		data[i + 10U] = m_node.at(i);

	for (unsigned int i = 0U; i < 14U; i++)
		data[i + 20U] = m_name.at(i);

	if (m_reflector == 0) {
		data[34U] = '1';
		data[35U] = '2';

		data[57U] = '0';
		data[58U] = '0';
		data[59U] = '0';
	} else {
		data[34U] = '1';
		data[35U] = '5';
		
		// id name count desc
		char buf[20];
		char buf1[20];
		
		sprintf(buf,"%05d",m_reflector);
		::memcpy(data+36U,buf,5U);
		
		if (m_reflector>99999U) sprintf(buf1,"CALL %d",m_reflector);
		else if (m_reflector==9U) strcpy(buf1,"LOCAL");		
		else if (m_reflector==9990U) strcpy(buf1,"PARROT");
		else if (m_reflector==4000U) strcpy(buf1,"UNLINK");
		else sprintf(buf1,"TG %d",m_reflector);		

		int i=strlen(buf1);
		while (i<16) {
			buf1[i]=' ';
			i++;
		}
		buf1[i]=0;
		::memcpy(data+41U,buf1,16U);
		::memcpy(data+57U,"000",3U);
		::memcpy(data+70U,"Descripcion   ",14U);		

	}

	unsigned int offset;
	char sign;
	if (m_txFrequency >= m_rxFrequency) {
		offset = m_txFrequency - m_rxFrequency;
		sign = '-';
	} else {
		offset = m_rxFrequency - m_txFrequency;
		sign = '+';
	}

	unsigned int freqHz = m_txFrequency % 1000000U;
	unsigned int freqkHz = (freqHz + 500U) / 1000U;

	char freq[30U];
	::sprintf(freq, "%05u.%03u000%c%03u.%06u", m_txFrequency / 1000000U, freqkHz, sign, offset / 1000000U, offset % 1000000U);

	for (unsigned int i = 0U; i < 23U; i++)
		data[i + 84U] = freq[i];

	data[127U] = 0x03U;			// End of data marker
	data[128U] = CCRC::addCRC(data, 128U);

	CUtils::dump(1U, "DX Reply", data, 129U);

	createReply(data, 129U);

	m_seqNo++;
}

void CWiresX::sendConnectReply(unsigned int reflector)
{
	m_reflector=reflector;
	assert(m_reflector != 0);

	unsigned char data[110U];
	::memset(data, 0x00U, 110U);
	::memset(data, ' ', 90U);

	data[0U] = m_seqNo;

	for (unsigned int i = 0U; i < 4U; i++)
		data[i + 1U] = CONN_RESP[i];

	for (unsigned int i = 0U; i < 5U; i++)
		data[i + 5U] = m_id.at(i);

	for (unsigned int i = 0U; i < 10U; i++)
		data[i + 10U] = m_node.at(i);

	for (unsigned int i = 0U; i < 14U; i++)
		data[i + 20U] = m_name.at(i);

	data[34U] = '1';
	data[35U] = '5';    

	// id name count desc
	char buf[20U];
	char buf1[20U];
	
	sprintf(buf,"%05d",m_reflector);
	::memcpy(data+36U,buf,5U);
	if (m_reflector>99999U) sprintf(buf1,"CALL %d",m_reflector);
	else if (m_reflector==9U) strcpy(buf1,"LOCAL");
	else if (m_reflector==9990U) strcpy(buf1,"PARROT");
	else if (m_reflector==4000U) strcpy(buf1,"UNLINK");
	else sprintf(buf1,"TG %d",m_reflector);
	
	int i=strlen(buf1);
	while (i<16) {
		buf1[i]=' ';
		i++;
	}
	buf1[i]=0;
	::memcpy(data+41U,buf1,16U);
	::memcpy(data+57U,"000",3U);
	::memcpy(data+70U,"Descripcion   ",14U);

	data[84U] = '0';
	data[85U] = '0';
	data[86U] = '0';
	data[87U] = '0';
	data[88U] = '0';

	data[89U] = 0x03U;			// End of data marker
	data[90U] = CCRC::addCRC(data, 90U);

	CUtils::dump(1U, "CONNECT Reply", data, 91U);

	createReply(data, 91U);

	m_seqNo++;
}

void CWiresX::sendDisconnectReply()
{
	unsigned char data[110U];
	::memset(data, 0x00U, 110U);
	::memset(data, ' ', 90U);

	data[0U] = m_seqNo;

	for (unsigned int i = 0U; i < 4U; i++)
		data[i + 1U] = DISC_RESP[i];

	for (unsigned int i = 0U; i < 5U; i++)
		data[i + 5U] = m_id.at(i);

	for (unsigned int i = 0U; i < 10U; i++)
		data[i + 10U] = m_node.at(i);

	for (unsigned int i = 0U; i < 14U; i++)
		data[i + 20U] = m_name.at(i);

	data[34U] = '1';
	data[35U] = '2';

	data[57U] = '0';
	data[58U] = '0';
	data[59U] = '0';

	data[89U] = 0x03U;			// End of data marker
	data[90U] = CCRC::addCRC(data, 90U);

	CUtils::dump(1U, "DISCONNECT Reply", data, 91U);

	createReply(data, 91U);

	m_seqNo++;
}

void CWiresX::sendAllReply()
{
/* //	if (m_start == 0U)
	//	m_reflectors.reload();

	//std::vector<CYSFReflector*>& curr = m_reflectors.current();

//	unsigned char data[1100U];
//	::memset(data, 0x00U, 1100U);

	data[0U] = m_seqNo;

	for (unsigned int i = 0U; i < 4U; i++)
		data[i + 1U] = ALL_RESP[i];

	data[5U] = '2';
	data[6U] = '1';

	for (unsigned int i = 0U; i < 5U; i++)
		data[i + 7U] = m_id.at(i);

	for (unsigned int i = 0U; i < 10U; i++)
		data[i + 12U] = m_node.at(i);

	unsigned int total = curr.size();
	if (total > 999U) total = 999U;

	unsigned int n = curr.size() - m_start;
	if (n > 20U) n = 20U;

	::sprintf((char*)(data + 22U), "%03u%03u", n, total);

	data[28U] = 0x0DU;

	unsigned int offset = 29U;
	for (unsigned int j = 0U; j < n; j++, offset += 50U) {
		CYSFReflector* refl = curr.at(j + m_start);

		::memset(data + offset, ' ', 50U);

		data[offset + 0U] = '5';

		for (unsigned int i = 0U; i < 5U; i++)
			data[i + offset + 1U] = refl->m_id.at(i);

		for (unsigned int i = 0U; i < 16U; i++)
			data[i + offset + 6U] = refl->m_name.at(i);

		for (unsigned int i = 0U; i < 3U; i++)
			data[i + offset + 22U] = refl->m_count.at(i);

		for (unsigned int i = 0U; i < 10U; i++)
			data[i + offset + 25U] = ' ';

		for (unsigned int i = 0U; i < 14U; i++)
			data[i + offset + 35U] = refl->m_desc.at(i);

		data[offset + 49U] = 0x0DU;
	}

	unsigned int k = 1029U - offset;
	for(unsigned int i = 0U; i < k; i++)
		data[i + offset] = 0x20U;
	
	offset += k;
    
	data[offset + 0U] = 0x03U;			// End of data marker
	data[offset + 1U] = CCRC::addCRC(data, offset + 1U);

	CUtils::dump(1U, "ALL Reply", data, offset + 2U);

	createReply(data, offset + 2U);

	m_seqNo++; */
}

void CWiresX::sendSearchReply()
{
//	if (m_search.size() == 0U) {
	//	sendSearchNotFoundReply();
	//	return;
//	}

	//std::vector<CYSFReflector*>& search = m_reflectors.search(m_search);
	//if (search.size() == 0U) {
		sendSearchNotFoundReply();
		return;
	/* }

	unsigned char data[1100U];
	::memset(data, 0x00U, 1100U);

	data[0U] = m_seqNo;

	for (unsigned int i = 0U; i < 4U; i++)
		data[i + 1U] = ALL_RESP[i];

	data[5U] = '0';
	data[6U] = '2';

	for (unsigned int i = 0U; i < 5U; i++)
		data[i + 7U] = m_id.at(i);

	for (unsigned int i = 0U; i < 10U; i++)
		data[i + 12U] = m_node.at(i);

	data[22U] = '1';

	unsigned int total = search.size();
	if (total > 999U) total = 999U;

	unsigned int n = search.size();
	if (n > 20U) n = 20U;

	::sprintf((char*)(data + 23U), "%02u%03u", n, total);

	data[28U] = 0x0DU;

	unsigned int offset = 29U;
	for (unsigned int j = 0U; j < n; j++, offset += 50U) {
		CYSFReflector* refl = search.at(j);

		::memset(data + offset, ' ', 50U);

		data[offset + 0U] = '1';

		for (unsigned int i = 0U; i < 5U; i++)
			data[i + offset + 1U] = refl->m_id.at(i);

		for (unsigned int i = 0U; i < 16U; i++)
			data[i + offset + 6U] = refl->m_name.at(i);

		for (unsigned int i = 0U; i < 3U; i++)
			data[i + offset + 22U] = refl->m_count.at(i);

		for (unsigned int i = 0U; i < 10U; i++)
			data[i + offset + 25U] = ' ';

		for (unsigned int i = 0U; i < 14U; i++)
			data[i + offset + 35U] = refl->m_desc.at(i);

		data[offset + 49U] = 0x0DU;
	}

	data[offset + 0U] = 0x03U;			// End of data marker
	data[offset + 1U] = CCRC::addCRC(data, offset + 1U);

	CUtils::dump(1U, "SEARCH Reply", data, offset + 2U);

	createReply(data, offset + 2U);

	m_seqNo++; */
}

void CWiresX::sendSearchNotFoundReply()
{
	unsigned char data[70U];
	::memset(data, 0x00U, 70U);

	data[0U] = m_seqNo;

	for (unsigned int i = 0U; i < 4U; i++)
		data[i + 1U] = ALL_RESP[i];

	data[5U] = '0';
	data[6U] = '1';

	for (unsigned int i = 0U; i < 5U; i++)
		data[i + 7U] = m_id.at(i);

	for (unsigned int i = 0U; i < 10U; i++)
		data[i + 12U] = m_node.at(i);

	data[22U] = '1';
	data[23U] = '0';
	data[24U] = '0';
	data[25U] = '0';
	data[26U] = '0';
	data[27U] = '0';

	data[28U] = 0x0DU;

	data[29U] = 0x03U;			// End of data marker
	data[30U] = CCRC::addCRC(data, 30U);

	CUtils::dump(1U, "SEARCH Reply", data, 31U);

	createReply(data, 31U);

	m_seqNo++;
}
