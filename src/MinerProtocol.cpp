//  cryptoport.io Burst Pool Miner
//
//  Created by Uray Meiviar < uraymeiviar@gmail.com > 2014
//  donation :
//
//  [Burst  ] BURST-8E8K-WQ2F-ZDZ5-FQWHX
//  [Bitcoin] 1UrayjqRjSJjuouhJnkczy5AuMqJGRK4b

#include "MinerProtocol.h"
#include "MinerLogger.h"
#include "MinerConfig.h"
#include "nxt/nxt_address.h"
#include "rapidjson/document.h"
#include "Miner.h"
#include "MinerUtil.h"

void Burst::MinerSocket::setRemote(const std::string& ip, size_t port, size_t defaultTimeout)
{
	inet_pton(AF_INET, ip.c_str(), &this->remoteAddr.sin_addr);
	this->remoteAddr.sin_family = AF_INET;
	this->remoteAddr.sin_port   = htons(static_cast<u_short>(port));

	this->socketTimeout.tv_sec  = static_cast<long>(defaultTimeout);
	this->socketTimeout.tv_usec = static_cast<long>(defaultTimeout) * 1000;
}

std::string Burst::MinerSocket::httpRequest(const std::string& method,
											const std::string& url,
											const std::string& body,
											const std::string& header)
{
	std::string response = "";
	memset(static_cast<void*>(this->readBuffer), 0, readBufferSize);
	auto sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

#ifdef WIN32
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&this->socketTimeout.tv_usec), sizeof(this->socketTimeout.tv_usec));
#else
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&this->socketTimeout), sizeof(struct timeval));
#endif

	if (connect(sock, reinterpret_cast<struct sockaddr*>(&this->remoteAddr), sizeof(struct sockaddr_in)) == -1)
	{
		MinerLogger::write("unable to connect to remote host", TextType::Error);
	}
	else
	{
		auto request = method + " ";
		request += url + " HTTP/1.0\r\n";
		request += header + "\r\n\r\n";
		request += body;

		if (send(sock, request.c_str(), static_cast<int>(request.length()),MSG_NOSIGNAL) > 0)
		{
			//shutdown(sock,SHUT_WR);

			auto totalBytesRead = 0;
			int bytesRead;

			do
			{
				bytesRead = static_cast<int>(recv(sock, &this->readBuffer[totalBytesRead],
												  readBufferSize - totalBytesRead - 1, 0));
				if (bytesRead > 0)
				{
					totalBytesRead += bytesRead;
					this->readBuffer[totalBytesRead] = 0;
					response += std::string(static_cast<char*>(this->readBuffer));
					totalBytesRead = 0;
					this->readBuffer[totalBytesRead] = 0;
				}
			}
			while ((bytesRead > 0) && (totalBytesRead < static_cast<int>(readBufferSize) - 1));

			shutdown(sock,SHUT_RDWR);
			closesocket(sock);

			if (response.length() > 0)
			{
				auto httpBodyPos = response.find("\r\n\r\n");
				response = response.substr(httpBodyPos + 4, response.length() - (httpBodyPos + 4));
			}
		}
	}

	return response;
}

std::string Burst::MinerSocket::httpPost(const std::string& url, const std::string& body, const std::string& header)
{
	std::string headerAll = "Connection: close";
	headerAll += "\r\nX-Miner: uray-creepsky";
	headerAll += header;
	//header = "Content-Type: application/x-www-form-urlencoded\r\n";
	//header = "Content-Length: "+std::to_string(body.length())+"\r\n";
			
	return this->httpRequest("POST", url, body, headerAll);
}

void Burst::MinerSocket::httpRequestAsync(const std::string& method,
										  const std::string& url,
										  const std::string& body,
										  const std::string& header,
										  std::function<void (std::string)> responseCallback)
{
	std::string response = httpRequest(method, url, body, header);
	responseCallback(response);
}

void Burst::MinerSocket::httpPostAsync(const std::string& url, const std::string& body,
									   std::function<void (std::string)> responseCallback)
{
	std::string //header = "Content-Type: application/x-www-form-urlencoded\r\n";
			//header = "Content-Length: "+std::to_string(body.length())+"\r\n";
			header = "Connection: close";
	std::thread requestThread(&MinerSocket::httpRequestAsync, this, "POST", url, body, header, responseCallback);
	requestThread.detach();
}

void Burst::MinerSocket::httpGetAsync(const std::string& url,
									  std::function<void (std::string)> responseCallback)
{
	std::string header = "Connection: close";
	std::thread requestThread(&MinerSocket::httpRequestAsync, this, "GET", url, "", header, responseCallback);
	requestThread.detach();
}

std::string Burst::MinerSocket::httpGet(const std::string& url)
{
	std::string header = "Connection: close";
	return httpRequest("GET", url, "", header);
}

Burst::MinerProtocol::MinerProtocol()
	: miner(nullptr)
{
	this->running = false;
	this->currentBaseTarget = 0;
	this->currentBlockHeight = 0;
}

Burst::MinerProtocol::~MinerProtocol()
{
	this->stop();
}

void Burst::MinerProtocol::stop()
{
	this->running = false;
}

bool Burst::MinerProtocol::run(Miner* miner)
{
	auto config = *miner->getConfig();
	this->miner = miner;
	auto remoteIP = resolveHostname(config.poolHost);

	if (remoteIP != "")
	{
		MinerLogger::write("Submission Max Delay : " + std::to_string(config.submissionMaxDelay), TextType::System);
		MinerLogger::write("Submission Max Retry : " + std::to_string(config.submissionMaxRetry), TextType::System);
		MinerLogger::write("Buffer Size : " + std::to_string(config.maxBufferSizeMB) + " MB", TextType::System);
		MinerLogger::write("Pool Host : " + config.poolHost + ":" + std::to_string(config.poolPort) + " (" + remoteIP + ")", TextType::System);

		this->running = true;
		this->miningInfoSocket.setRemote(remoteIP, config.poolPort);
		this->nonceSubmitterSocket.setRemote(remoteIP, config.poolPort);

		while (this->running)
		{
			this->getMiningInfo();
			std::this_thread::sleep_for(std::chrono::seconds(3));
		}
	}

	return false;
}

Burst::SubmitResponse Burst::MinerProtocol::submitNonce(uint64_t nonce, uint64_t accountId, uint64_t& deadline)
{
	NxtAddress addr(accountId);

	if (targetDeadline > 0 && deadline > targetDeadline)
		return SubmitResponse::TooHigh;

	//MinerLogger::write("submitting nonce " + std::to_string(nonce) + " for " + addr.to_string());
	MinerLogger::write(addr.to_string() + ": nonce submitted (" + deadlineFormat(deadline) + ")", TextType::Ok);

	auto request = "requestType=submitNonce&nonce=" + std::to_string(nonce) + "&accountId=" + std::to_string(accountId) + "&secretPhrase=cryptoport";
	auto url = "/burst?" + request;
	auto tryCount = 0u;
	std::string response = "";

	do
	{
		response = this->nonceSubmitterSocket.httpPost(url, "",
			"\r\nX-Capacity: " + std::to_string(miner->getConfig()->getTotalPlotsize() / 1024 / 1024 / 1024));
		++tryCount;
	}
	while (response.empty() && tryCount < this->miner->getConfig()->submissionMaxRetry);

	//MinerLogger::write(response);
	rapidjson::Document body;
	body.Parse<0>(response.c_str());

	if (body.GetParseError() == nullptr)
	{
		if (body.HasMember("deadline"))
		{
			deadline = body["deadline"].GetUint64();
			return SubmitResponse::Submitted;
		}
		
		if (body.HasMember("errorCode"))
		{
			MinerLogger::write(std::string("error: ") + body["errorDescription"].GetString(), TextType::Error);
			// we send true so we dont send it again and again
			return SubmitResponse::Error;
		}
		
		MinerLogger::write(response, TextType::Error);
		return SubmitResponse::Error;
	}

	return SubmitResponse::None;
}

void Burst::MinerProtocol::getMiningInfo()
{
	auto response = this->miningInfoSocket.httpPost("/burst?requestType=getMiningInfo", "");

	if (response.length() > 0)
	{
		rapidjson::Document body;
		body.Parse<0>(response.c_str());
		
		if (body.GetParseError() == nullptr)
		{
			if (body.HasMember("baseTarget"))
			{
				std::string baseTargetStr = body["baseTarget"].GetString();
				this->currentBaseTarget = std::stoull(baseTargetStr);
			}
			
			if (body.HasMember("generationSignature"))
			{
				this->gensig = body["generationSignature"].GetString();
			}

			if (body.HasMember("height"))
			{
				std::string newBlockHeightStr = body["height"].GetString();
				auto newBlockHeight = std::stoull(newBlockHeightStr);

				if (newBlockHeight > this->currentBlockHeight)
					this->miner->updateGensig(this->gensig, newBlockHeight, this->currentBaseTarget);

				this->currentBlockHeight = newBlockHeight;
			}

			if (body.HasMember("targetDeadline"))
			{
				targetDeadline = body["targetDeadline"].GetUint64();
			}
		}
	}
}

std::string Burst::MinerProtocol::resolveHostname(const std::string host)
{
	struct hostent* he;
	struct in_addr** addr_list;
	int i;

	// MinerLogger::write("resolving hostname "+host);

	if ((he = gethostbyname(host.c_str())) == nullptr)
	{
		MinerLogger::write("error while resolving hostname", TextType::Error);
		return "";
	}

	addr_list = reinterpret_cast<struct in_addr **>(he->h_addr_list);
	char inetAddrStr[64];

	for (i = 0; addr_list[i] != nullptr;)
	{
		auto ip = std::string(inet_ntop(AF_INET, addr_list[i], inetAddrStr, 64));
		//MinerLogger::write("Remote IP " + ip);
		return ip;
	}

	MinerLogger::write("can not resolve hostname " + host, TextType::Error);
	return "";
}