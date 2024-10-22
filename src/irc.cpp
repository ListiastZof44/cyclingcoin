// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"
#include "irc.h"
#include "net.h"
#include "init.h"
#include "strlcpy.h"
#include "base58.h"
#include <inttypes.h>

using namespace std;
using namespace boost;


int nGotIRCAddresses = 0;

void ThreadIRCSeed2();




#pragma pack(push, 1)
struct ircaddr
{
    struct in_addr ip;
    short port;
};
#pragma pack(pop)

string EncodeAddress(const CService& addr)
{
    struct ircaddr tmp;
    if (addr.GetInAddr(&tmp.ip))
    {
        tmp.port = htons(addr.GetPort());

        vector<unsigned char> vch(UBEGIN(tmp), UEND(tmp));
        return string("u") + EncodeBase58Check(vch);
    }
    return "";
}

bool DecodeAddress(string str, CService& addr)
{
    vector<unsigned char> vch;
    if (!DecodeBase58Check(str.substr(1), vch))
        return false;

    struct ircaddr tmp;
    if (vch.size() != sizeof(tmp))
        return false;
    memcpy(&tmp, &vch[0], sizeof(tmp));

    addr = CService(tmp.ip, ntohs(tmp.port));
    return true;
}






static bool Send(SOCKET hSocket, const char* pszSend)
{
    boost::this_thread::interruption_point();
    if (strstr(pszSend, "PONG") != pszSend)
        LogPrintf("IRC SENDING: %s\n", pszSend);
    const char* psz = pszSend;
    const char* pszEnd = psz + strlen(psz);
    while (psz < pszEnd)
    {
	boost::this_thread::interruption_point();
        int ret = send(hSocket, psz, pszEnd - psz, MSG_NOSIGNAL);
        if (ret < 0)
            return false;
        psz += ret;
    }
    return true;
}

bool RecvLineIRC(SOCKET hSocket, string& strLine)
{
    if(ShutdownRequested())
	return false;

    boost::this_thread::interruption_point();
    while (!ShutdownRequested())
    {
	boost::this_thread::interruption_point();
	
        bool fRet = RecvLine(hSocket, strLine);
	
	if(ShutdownRequested())
	{
	    LogPrintf("RecvLineIRC: shutdown requested\n");
	    return false;
	}

	boost::this_thread::interruption_point();
        if (fRet)
        {            
            vector<string> vWords;
            ParseString(strLine, ' ', vWords);
            if (vWords.size() >= 1 && vWords[0] == "PING")
            {
                strLine[1] = 'O';
                strLine += '\r';
                Send(hSocket, strLine.c_str());
                continue;
            };
        };
        return fRet;
    };

    return false;
}

int RecvUntil(SOCKET hSocket, const char* psz1, const char* psz2=NULL, const char* psz3=NULL, const char* psz4=NULL)
{
    boost::this_thread::interruption_point();
    while (!ShutdownRequested())
    {
	if(ShutdownRequested())
	    break;

	boost::this_thread::interruption_point();
        string strLine;
        strLine.reserve(10000);
        if (!RecvLineIRC(hSocket, strLine))
            return 0;
        LogPrintf("IRC %s\n", strLine.c_str());
        if (psz1 && strLine.find(psz1) != string::npos)
            return 1;
        if (psz2 && strLine.find(psz2) != string::npos)
            return 2;
        if (psz3 && strLine.find(psz3) != string::npos)
            return 3;
        if (psz4 && strLine.find(psz4) != string::npos)
            return 4;
    }

    return 0;
}

bool Wait(int nSeconds)
{
    if(ShutdownRequested())
	return false;

    boost::this_thread::interruption_point();
    LogPrintf("IRC waiting %d seconds to reconnect\n", nSeconds);
    for (int i = 0; i < nSeconds; i++)
    {
	boost::this_thread::interruption_point();
	if(ShutdownRequested())
	    return false;

        MilliSleep(1000);
    }
    return true;
}

bool RecvCodeLine(SOCKET hSocket, const char* psz1, string& strRet)
{
    boost::this_thread::interruption_point();
    strRet.clear();
    while (!ShutdownRequested())
    {
	if(ShutdownRequested())
	    return false;

	boost::this_thread::interruption_point();

        string strLine;
        if (!RecvLineIRC(hSocket, strLine))
            return false;

        vector<string> vWords;
        ParseString(strLine, ' ', vWords);
        if (vWords.size() < 2)
            continue;

        if (vWords[1] == psz1)
        {
            LogPrintf("IRC %s\n", strLine.c_str());
            strRet = strLine;
            return true;
        }
    }

    return false;
}

bool GetIPFromIRC(SOCKET hSocket, string strMyName, CNetAddr& ipRet)
{
    boost::this_thread::interruption_point();
    Send(hSocket, strprintf("USERHOST %s\r", strMyName.c_str()).c_str());

    string strLine;
    if (!RecvCodeLine(hSocket, "302", strLine))
        return false;

    vector<string> vWords;
    ParseString(strLine, ' ', vWords);
    if (vWords.size() < 4)
        return false;

    string str = vWords[3];
    if (str.rfind("@") == string::npos)
        return false;
    string strHost = str.substr(str.rfind("@")+1);

    // Hybrid IRC used by lfnet always returns IP when you userhost yourself,
    // but in case another IRC is ever used this should work.
    LogPrintf("GetIPFromIRC() got userhost %s\n", strHost.c_str());
    CNetAddr addr(strHost, true);
    if (!addr.IsValid())
        return false;
    ipRet = addr;

    return true;
}



void ThreadIRCSeed()
{
    // Make this thread recognisable as the IRC seeding thread
    RenameThread("cyclingcoin-ircseed");

    try
    {
        ThreadIRCSeed2();
    } 
    catch (boost::thread_interrupted)
    {
	LogPrintf("irc thread interrupted.\n");
    }
    catch (std::exception& e)
    {
        PrintExceptionContinue(&e, "ThreadIRCSeed()");
    } catch (...)
    {
        PrintExceptionContinue(NULL, "ThreadIRCSeed()");
    };
    
    LogPrintf("ThreadIRCSeed exited\n");
}

void ThreadIRCSeed2()
{
    LogPrintf("ThreadIRCSeed2 start\n");

    LogPrintf("Check if not ipv4\n");
    // Don't connect to IRC if we won't use IPv4 connections.
    if (IsLimited(NET_IPV4))
        return;

    LogPrintf("Check if connect or nolisten\n");
    // ... or if we won't make outbound connections and won't accept inbound ones.
    if (mapArgs.count("-connect") && fNoListen)
        return;

    LogPrintf("Check if irc enabled\n");
    // ... or if IRC is not enabled.
    if (!GetBoolArg("-irc", true))
        return;

    LogPrintf("ThreadIRCSeed started\n");
    int nErrorWait = 10;
    int nRetryWait = 10;
    int nNameRetry = 0;

    while (!ShutdownRequested())
    {
        boost::this_thread::interruption_point();

        CService addrConnect("92.243.23.21", 6667); // irc.lfnet.org

        CService addrIRC("irc.lfnet.org", 6667, true);
        if (addrIRC.IsValid())
            addrConnect = addrIRC;

        SOCKET hSocket;
	bool bProxyConnectionFailed = false;
	int ntimeout = 1000;
        if (!ConnectSocket(addrConnect, hSocket, ntimeout, &bProxyConnectionFailed))
        {
	    boost::this_thread::interruption_point();
            LogPrintf("IRC connect failed\n");
            nErrorWait = nErrorWait * 11 / 10;
            if (Wait(nErrorWait += 60))
                continue;
            else
                return;
        };

        if (!RecvUntil(hSocket, "Found your hostname", "using your IP address instead", "Couldn't look up your hostname", "ignoring hostname"))
        {
	    boost::this_thread::interruption_point();
            closesocket(hSocket);
            hSocket = INVALID_SOCKET;
            nErrorWait = nErrorWait * 11 / 10;
            if (Wait(nErrorWait += 60))
                continue;
            else
                return;
        };

        CNetAddr addrIPv4("1.2.3.4"); // arbitrary IPv4 address to make GetLocal prefer IPv4 addresses
        CService addrLocal;
        string strMyName;
        // Don't use our IP as our nick if we're not listening
        // or if it keeps failing because the nick is already in use.
        if (!fNoListen && GetLocal(addrLocal, &addrIPv4) && nNameRetry<3)
            strMyName = EncodeAddress(GetLocalAddress(&addrConnect));
        if (strMyName == "")
            strMyName = strprintf("x%llu", GetRand(1000000000));

        Send(hSocket, strprintf("NICK %s\r", strMyName.c_str()).c_str());
        Send(hSocket, strprintf("USER %s 8 * : %s\r", strMyName.c_str(), strMyName.c_str()).c_str());

        int nRet = RecvUntil(hSocket, " 004 ", " 433 ");
        if (nRet != 1)
        {
            closesocket(hSocket);
            hSocket = INVALID_SOCKET;
            if (nRet == 2)
            {
                LogPrintf("IRC name already in use\n");
                nNameRetry++;
                Wait(10);
                continue;
            };
            nErrorWait = nErrorWait * 11 / 10;
	    boost::this_thread::interruption_point();
            if (Wait(nErrorWait += 60))
                continue;
            else
                return;
        };
        
        nNameRetry = 0;
        MilliSleep(500);

        // Get our external IP from the IRC server and re-nick before joining the channel
        CNetAddr addrFromIRC;
        if (GetIPFromIRC(hSocket, strMyName, addrFromIRC))
        {
            LogPrintf("GetIPFromIRC() returned %s\n", addrFromIRC.ToString().c_str());
            // Don't use our IP as our nick if we're not listening
            if (!fNoListen && addrFromIRC.IsRoutable())
            {
                // IRC lets you to re-nick
                AddLocal(addrFromIRC, LOCAL_IRC);
                strMyName = EncodeAddress(GetLocalAddress(&addrConnect));
                Send(hSocket, strprintf("NICK %s\r", strMyName.c_str()).c_str());
            };
        };

        if (TestNet())
        {
            Send(hSocket, "JOIN #cyclingcoincoinTEST\r");
            Send(hSocket, "WHO #cyclingcoincoinTEST\r");
        } else
        {
	    LogPrintf("JOIN #cyclingcoincoin\n");
            // randomly join #cyclingcoincoin00-#cyclingcoincoin05
            //int channel_number = GetRandInt(5);

            // Channel number is always 0 for initial release
            int channel_number = 0;
            Send(hSocket, strprintf("JOIN #cyclingcoincoin%02d\r", channel_number).c_str());
            Send(hSocket, strprintf("WHO #cyclingcoincoin%02d\r", channel_number).c_str());
        };

        int64_t nStart = GetTime();
        string strLine;
        strLine.reserve(10000);
        while (!ShutdownRequested() && RecvLineIRC(hSocket, strLine))
        {
	    if(ShutdownRequested())
		break;

            boost::this_thread::interruption_point();

            if (strLine.empty() || strLine.size() > 900 || strLine[0] != ':')
                continue;

            vector<string> vWords;
            ParseString(strLine, ' ', vWords);
            if (vWords.size() < 2)
                continue;

            char pszName[10000];
            pszName[0] = '\0';

            if (vWords[1] == "352" && vWords.size() >= 8)
            {
                // index 7 is limited to 16 characters
                // could get full length name at index 10, but would be different from join messages
                strlcpy(pszName, vWords[7].c_str(), sizeof(pszName));
                LogPrintf("IRC got who\n");
            };

            if (vWords[1] == "JOIN" && vWords[0].size() > 1)
            {
                // :username!username@50000007.F000000B.90000002.IP JOIN :#channelname
                strlcpy(pszName, vWords[0].c_str() + 1, sizeof(pszName));
                if (strchr(pszName, '!'))
                    *strchr(pszName, '!') = '\0';
                LogPrintf("IRC got join\n");
            };

            if (pszName[0] == 'u')
            {
                CAddress addr;
                if (DecodeAddress(pszName, addr))
                {
                    addr.nTime = GetAdjustedTime();
                    if (addrman.Add(addr, addrConnect, 51 * 60))
                        LogPrintf("IRC got new address: %s\n", addr.ToString().c_str());
                    nGotIRCAddresses++;
                } else
                {
                    LogPrintf("IRC decode failed\n");
                };
            };

	    if(ShutdownRequested())
		break;

	    boost::this_thread::interruption_point();
        };

	if(ShutdownRequested())
	    break;

        closesocket(hSocket);
        hSocket = INVALID_SOCKET;

        if (GetTime() - nStart > 20 * 60)
        {
            nErrorWait /= 3;
            nRetryWait /= 3;
        };

        nRetryWait = nRetryWait * 11 / 10;
	boost::this_thread::interruption_point();

 	if(ShutdownRequested())
	    break;;

        if (!Wait(nRetryWait += 60))
            return;
    };
}










#ifdef TEST
int main(int argc, char *argv[])
{
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2,2), &wsadata) != NO_ERROR)
    {
        LogPrintf("Error at WSAStartup()\n");
        return false;
    }

    ThreadIRCSeed(NULL);

    WSACleanup();
    return 0;
}
#endif
