#include "smtpclientbase.h"
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include "base64.h"
#include "errorresolver.h"
#include "message.h"
#include "messageaddress.h"
#include "serverauthoptions.h"
#include "smtpclienterrors.h"
#include "smtpserverstatuscodes.h"
#include "socketerrors.h"
#include "stringutils.h"
#ifdef _WIN32
    #include <WinSock2.h>
    #include <ws2tcpip.h>
    #include <BaseTsd.h>
    typedef SSIZE_T ssize_t;
    #include <windows.h>
    #include <WinNT.h>
    constexpr auto sleep = Sleep;
#else
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

using namespace std::literals::string_literals;
using namespace jed_utils;

SMTPClientBase::SMTPClientBase(const char *pServerName, unsigned int pPort)
    : mServerName(nullptr),
      mPort(pPort),
      mCommunicationLog(nullptr),
      mLastServerResponse(nullptr),
      mCommandTimeOut(5),
      mLastSocketErrNo(0),
      mAuthOptions(nullptr),
      mCredential(nullptr),
      mKeepUsingBaseSendCommands(false),
      sendCommandPtr(&SMTPClientBase::sendCommand),
      sendCommandWithFeedbackPtr(&SMTPClientBase::sendCommandWithFeedback) {
    std::string servername_str { pServerName == nullptr ? "" : pServerName };
    if (pServerName == nullptr || strcmp(pServerName, "") == 0  || StringUtils::trim(servername_str).empty()) {
        throw std::invalid_argument("Server name cannot be null or empty");
    }
    size_t server_name_len = strlen(pServerName);
    mServerName = new char[server_name_len + 1];
    strncpy(mServerName, pServerName, server_name_len);
    mServerName[server_name_len] = '\0';
}

SMTPClientBase::~SMTPClientBase() {
    delete[] mServerName;
    mServerName = nullptr;
    delete[] mCommunicationLog;
    mCommunicationLog = nullptr;
    delete[] mLastServerResponse;
    mLastServerResponse = nullptr;
    delete mAuthOptions;
    mAuthOptions = nullptr;
    mCredential = nullptr;
}

// Copy constructor
SMTPClientBase::SMTPClientBase(const SMTPClientBase& other)
    : mServerName(new char[strlen(other.mServerName) + 1]),
      mPort(other.mPort),
      mCommunicationLog(other.mCommunicationLog != nullptr ? new char[strlen(other.mCommunicationLog) + 1]: nullptr),
      mCommunicationLogSize(other.mCommunicationLogSize),
      mLastServerResponse(other.mLastServerResponse != nullptr ? new char[strlen(other.mLastServerResponse) + 1]: nullptr),
      mCommandTimeOut(other.mCommandTimeOut),
      mLastSocketErrNo(other.mLastSocketErrNo),
      mAuthOptions(other.mAuthOptions != nullptr ? new ServerAuthOptions(*other.mAuthOptions) : nullptr),
      mCredential(other.mCredential != nullptr ? new Credential(*other.mCredential) : nullptr),
      mSock(0),
      mKeepUsingBaseSendCommands(other.mKeepUsingBaseSendCommands),
      sendCommandPtr(&SMTPClientBase::sendCommand),
      sendCommandWithFeedbackPtr(&SMTPClientBase::sendCommandWithFeedback) {
    size_t server_name_len = strlen(other.mServerName);
    strncpy(mServerName, other.mServerName, server_name_len);
    mServerName[server_name_len] = '\0';
    if (mCommunicationLog != nullptr) {
        size_t communication_log_len = strlen(other.mCommunicationLog);
        strncpy(mCommunicationLog, other.mCommunicationLog, communication_log_len);
        mCommunicationLog[communication_log_len] = '\0';
    }

    if (mLastServerResponse != nullptr) {
        size_t last_server_response_len = strlen(other.mLastServerResponse);
        strncpy(mLastServerResponse, other.mLastServerResponse, last_server_response_len);
        mLastServerResponse[last_server_response_len] = '\0';
    }
    setKeepUsingBaseSendCommands(mKeepUsingBaseSendCommands);
}

// Assignment operator
SMTPClientBase& SMTPClientBase::operator=(const SMTPClientBase& other) {
    if (this != &other) {
        // mServerName
        delete[] mServerName;
        size_t server_name_len = strlen(other.mServerName);
        mServerName = new char[server_name_len + 1];
        strncpy(mServerName, other.mServerName, server_name_len);
        mServerName[server_name_len] = '\0';
        // mPort
        mPort = other.mPort;
        // mCommunicationLog
        mCommunicationLog = other.mCommunicationLog != nullptr ? new char[strlen(other.mCommunicationLog) + 1]: nullptr;
        if (mCommunicationLog != nullptr) {
            size_t communication_log_len = strlen(other.mCommunicationLog);
            strncpy(mCommunicationLog, other.mCommunicationLog, communication_log_len);
            mCommunicationLog[communication_log_len] = '\0';
        }
        mCommunicationLogSize = other.mCommunicationLogSize;
        // mLastServerResponse
        mLastServerResponse = other.mLastServerResponse != nullptr ? new char[strlen(other.mLastServerResponse) + 1]: nullptr;
        if (mLastServerResponse != nullptr) {
            size_t last_server_response_len = strlen(other.mLastServerResponse);
            strncpy(mLastServerResponse, other.mLastServerResponse, last_server_response_len);
            mLastServerResponse[last_server_response_len] = '\0';
        }
        mCommandTimeOut = other.mCommandTimeOut;
        mLastSocketErrNo = other.mLastSocketErrNo;

        delete mAuthOptions;
        mAuthOptions = other.mAuthOptions != nullptr ? new ServerAuthOptions(*other.mAuthOptions) : nullptr;
        // mCredential
        mCredential = other.mCredential != nullptr ? new Credential(*other.mCredential) : nullptr;
        mSock = 0;
        setKeepUsingBaseSendCommands(other.mKeepUsingBaseSendCommands);
    }
    return *this;
}

// Move constructor
SMTPClientBase::SMTPClientBase(SMTPClientBase&& other) noexcept
    : mServerName(other.mServerName),
      mPort(other.mPort),
      mCommunicationLog(other.mCommunicationLog),
      mCommunicationLogSize(other.mCommunicationLogSize),
      mLastServerResponse(other.mLastServerResponse),
      mCommandTimeOut(other.mCommandTimeOut),
      mLastSocketErrNo(other.mLastSocketErrNo),
      mAuthOptions(other.mAuthOptions),
      mCredential(other.mCredential),
      mSock(other.mSock),
      mKeepUsingBaseSendCommands(other.mKeepUsingBaseSendCommands),
      sendCommandPtr(&SMTPClientBase::sendCommand),
      sendCommandWithFeedbackPtr(&SMTPClientBase::sendCommandWithFeedback) {
    other.mServerName = nullptr;
    other.mPort = 0;
    other.mCommunicationLog = nullptr;
    other.mCommunicationLogSize = 0;
    other.mLastServerResponse = nullptr;
    other.mCommandTimeOut = 0;
    other.mLastSocketErrNo = 0;
    other.mAuthOptions = nullptr;
    other.mCredential = nullptr;
    other.mSock = 0;
    other.mKeepUsingBaseSendCommands = false;
    setKeepUsingBaseSendCommands(mKeepUsingBaseSendCommands);
}

// Move assignement operator
SMTPClientBase& SMTPClientBase::operator=(SMTPClientBase&& other) noexcept {
    if (this != &other) {
        delete[] mServerName;
        delete[] mCommunicationLog;
        delete[] mLastServerResponse;
        delete mAuthOptions;
        delete mCredential;
        // Copy the data pointer and its length from the source object.
        mServerName = other.mServerName;
        mPort = other.mPort;
        mCommunicationLog = other.mCommunicationLog;
        mCommunicationLogSize = other.mCommunicationLogSize;
        mLastServerResponse = other.mLastServerResponse;
        mCommandTimeOut = other.mCommandTimeOut;
        mLastSocketErrNo = other.mLastSocketErrNo;
        mAuthOptions = other.mAuthOptions;
        mCredential = other.mCredential;
        mSock = other.mSock;
        mKeepUsingBaseSendCommands = other.mKeepUsingBaseSendCommands;
        setKeepUsingBaseSendCommands(mKeepUsingBaseSendCommands);
        // Release the data pointer from the source object so that
        // the destructor does not free the memory multiple times.
        other.mServerName = nullptr;
        other.mPort = 0;
        other.mCommunicationLog = nullptr;
        other.mCommunicationLogSize = 0;
        other.mLastServerResponse = nullptr;
        other.mCommandTimeOut = 0;
        other.mLastSocketErrNo = 0;
        other.mAuthOptions = nullptr;
        other.mCredential = nullptr;
        other.mSock = 0;
        other.mKeepUsingBaseSendCommands = false;
    }
    return *this;
}

const char *SMTPClientBase::getServerName() const {
    return mServerName;
}

unsigned int SMTPClientBase::getServerPort() const {
    return mPort;
}

unsigned int SMTPClientBase::getCommandTimeout() const {
    return mCommandTimeOut;
}

const char *SMTPClientBase::getCommunicationLog() const {
    return mCommunicationLog == nullptr ? "" : mCommunicationLog;
}

const Credential *SMTPClientBase::getCredentials() const {
    return mCredential;
}

void SMTPClientBase::setServerPort(unsigned int pPort) {
    mPort = pPort;
}

void SMTPClientBase::setServerName(const char *pServerName) {
    std::string servername_str { pServerName == nullptr ? "" : pServerName };
    if (pServerName == nullptr || strcmp(pServerName, "") == 0  || StringUtils::trim(servername_str).empty()) {
        throw std::invalid_argument("Server name cannot be null or empty");
    }
    delete []mServerName;
    size_t server_name_len = strlen(pServerName);
    mServerName = new char[server_name_len + 1];
    strncpy(mServerName, pServerName, server_name_len);
    mServerName[server_name_len] = '\0';
}

void SMTPClientBase::setCommandTimeout(unsigned int pTimeOutInSeconds) {
    mCommandTimeOut = pTimeOutInSeconds;
}

void SMTPClientBase::setCredentials(const Credential &pCredential) {
    delete mCredential;
    mCredential = new Credential(pCredential);
}

void SMTPClientBase::setKeepUsingBaseSendCommands(bool pValue) {
    mKeepUsingBaseSendCommands = pValue;
    if (pValue) {
        sendCommandPtr = &SMTPClientBase::sendRawCommand;
        sendCommandWithFeedbackPtr = &SMTPClientBase::sendRawCommand;
    } else {
        sendCommandPtr = &SMTPClientBase::sendCommand;
        sendCommandWithFeedbackPtr = &SMTPClientBase::sendCommandWithFeedback;
    }
}

int SMTPClientBase::getSocketFileDescriptor() const {
    return mSock;
}

void SMTPClientBase::clearSocketFileDescriptor() {
    mSock = 0;
}

const char *SMTPClientBase::getLastServerResponse() const {
    return mLastServerResponse;
}

void SMTPClientBase::setLastSocketErrNo(int lastError) {
    mLastSocketErrNo = lastError;
}

void SMTPClientBase::setAuthenticationOptions(ServerAuthOptions *authOptions) {
    delete mAuthOptions;
    mAuthOptions = authOptions;
}

char *SMTPClientBase::getErrorMessage(int errorCode) {
    ErrorResolver errorResolver(errorCode);
    const char *errorMessageStr = errorResolver.getErrorMessage();
    size_t error_message_len = strlen(errorMessageStr);
    char *errorMessage = new char[error_message_len + 1];
    strncpy(errorMessage, errorMessageStr, error_message_len);
    errorMessage[error_message_len] = '\0';
    return errorMessage;
}

int SMTPClientBase::getErrorMessage_r(int errorCode,
        char *errorMessagePtr,
        const size_t maxLength) {
    ErrorResolver errorResolver(errorCode);
    const char *errorMessageStr = errorResolver.getErrorMessage();
    if (!errorMessagePtr || maxLength == 0) {
        return -1;
    }
    if (strlen(errorMessageStr) > maxLength-1) {
        strncpy(errorMessagePtr, errorMessageStr, maxLength-1);
        errorMessagePtr[maxLength-1] = '\0';
        return static_cast<int>(maxLength-1);
    }
    size_t error_message_len = strlen(errorMessageStr);
    strncpy(errorMessagePtr, errorMessageStr, error_message_len);
    errorMessagePtr[error_message_len] = '\0';
    return 0;
}

int SMTPClientBase::sendMail(const Message &pMsg) {
    int client_connect_ret_code = establishConnectionWithServer();
    if (client_connect_ret_code != 0) {
        return client_connect_ret_code;
    }

    int set_mail_recipients_ret_code = setMailRecipients(pMsg);
    if (set_mail_recipients_ret_code != 0) {
        return set_mail_recipients_ret_code;
    }

    int set_mail_headers_ret_code = setMailHeaders(pMsg);
    if (set_mail_headers_ret_code != 0) {
        return set_mail_headers_ret_code;
    }

    int set_mail_body_ret_code = setMailBody(pMsg);
    if (set_mail_body_ret_code != 0) {
        return set_mail_body_ret_code;
    }

    cleanup();
    return 0;
}


int SMTPClientBase::initializeSession() {
    delete[] mCommunicationLog;
    mCommunicationLog = new char[INITIAL_COMM_LOG_LENGTH];
    mCommunicationLogSize = INITIAL_COMM_LOG_LENGTH;
    mCommunicationLog[0] = '\0';

#ifdef _WIN32
    return initializeSessionWinSock();
#else
    return initializeSessionPOSIX();
#endif
}

#ifdef _WIN32
int SMTPClientBase::initializeSessionWinSock() {
    // Windows Sockets version
    WSADATA wsaData;
    WORD wVersionRequested = MAKEWORD(2, 2);
    int wsa_retVal = WSAStartup(wVersionRequested, &wsaData);
    if (wsa_retVal != 0) {
        std::stringstream ssError;
        setLastSocketErrNo(wsa_retVal);
        ssError << "WSAStartup failed with error: " << wsa_retVal;
        addCommunicationLogItem(ssError.str().c_str());
        return SOCKET_INIT_SESSION_WINSOCKET_STARTUP_ERROR;
    }
    mWSAStarted = true;
    struct addrinfo *result = nullptr;
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;  // use IPv4 or IPv6, whichever
    hints.ai_socktype = SOCK_STREAM;

    wsa_retVal = getaddrinfo(getServerName(), std::to_string(getServerPort()).c_str(), &hints, &result);
    if (wsa_retVal != 0) {
        addWSAMessageToCommunicationLog(wsa_retVal);
        setLastSocketErrNo(wsa_retVal);
        doWSACleanup();
        return SOCKET_INIT_SESSION_WINSOCKET_GETADDRINFO_ERROR;
    }

    mSock = static_cast<unsigned int>(socket(result->ai_family, result->ai_socktype, result->ai_protocol));
    if (mSock == INVALID_SOCKET) {
        int wsa_error = WSAGetLastError();
        addWSAMessageToCommunicationLog(wsa_error);
        setLastSocketErrNo(wsa_error);
        doWSACleanup();
        return SOCKET_INIT_SESSION_CREATION_ERROR;
    }
    std::stringstream ss;
    ss << "Trying to connect to " << getServerName() << " on port " << getServerPort();
    addCommunicationLogItem(ss.str().c_str());
    wsa_retVal = connect(mSock, result->ai_addr, static_cast<int>(result->ai_addrlen));
    if (wsa_retVal == SOCKET_ERROR) {
        int wsa_error = WSAGetLastError();
        addWSAMessageToCommunicationLog(wsa_error);
        setLastSocketErrNo(wsa_error);
        doWSACleanup();
        return SOCKET_INIT_SESSION_CONNECT_ERROR;
    }
    return 0;
}

bool SMTPClientBase::isWSAStarted() {
    return mWSAStarted;
};

void SMTPClientBase::setWSAStopped() {
    mWSAStarted = false;
};

void SMTPClientBase::addWSAMessageToCommunicationLog(const int errorCode) {
    LPTSTR formattedErrorMessage = nullptr;
    if(FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_MAX_WIDTH_MASK,
        nullptr,
        errorCode,
        0,
        (LPTSTR)&formattedErrorMessage,
        0,
        nullptr) != 0) {
        addCommunicationLogItem(formattedErrorMessage);
        LocalFree(formattedErrorMessage);
    }
}

void SMTPClientBase::doWSACleanup() {
    if (WSACleanup() != 0) {
        int wsa_retVal = WSAGetLastError();
        std::stringstream ssError;
        setLastSocketErrNo(wsa_retVal);
        ssError << "WSACleanup failed with error: " << wsa_retVal;
        addCommunicationLogItem(ssError.str().c_str());
    }
    mWSAStarted = false;
}

#else
int SMTPClientBase::initializeSessionPOSIX() {
    // POSIX socket version
    fd_set fdset;
    struct timeval tv;
    socklen_t lon;
    int valopt;
    mSock = socket(AF_INET, SOCK_STREAM, 0);
    if (mSock < 0) {
        setLastSocketErrNo(errno);
        addCommunicationLogItem(strerror(errno));
        return SOCKET_INIT_SESSION_CREATION_ERROR;
    }
    struct hostent *host = gethostbyname(getServerName());
    if (!host || host->h_length < 0) {
        return SOCKET_INIT_SESSION_GETHOSTBYNAME_ERROR;
    }
    struct sockaddr_in saddr_in {};
    saddr_in.sin_family = AF_INET;
    saddr_in.sin_port = htons(static_cast<u_short>(getServerPort()));
    saddr_in.sin_addr.s_addr = 0;
    memcpy(reinterpret_cast<char*>(&(saddr_in.sin_addr)),
           host->h_addr,
           static_cast<size_t>(host->h_length));
    // Set non-blocking
    if (int socketSetFlagsResult = setSocketToNonBlockingPOSIX() != 0) {
        return socketSetFlagsResult;
    }
    std::stringstream ss;
    ss << "Trying to connect to " << getServerName() << " on port " << getServerPort();
    addCommunicationLogItem(ss.str().c_str());
    int res = connect(mSock, reinterpret_cast<struct sockaddr*>(&saddr_in), sizeof(saddr_in));
    if (res < 0) {
        if (errno == EINPROGRESS) {
            do {
                tv.tv_sec = mCommandTimeOut;
                tv.tv_usec = 0;
                FD_ZERO(&fdset);
                FD_SET(mSock, &fdset);
                res = select(mSock+1, NULL, &fdset, NULL, &tv);
                if (res < 0 && errno != EINTR) {
                    setLastSocketErrNo(errno);
                    addCommunicationLogItem(strerror(errno));
                    return SOCKET_INIT_SESSION_CONNECT_ERROR;
                } else if (res > 0) {
                    // Socket selected for write
                    lon = sizeof(int);
                    if (getsockopt(mSock,
                                   SOL_SOCKET,
                                   SO_ERROR,
                                   static_cast<void*>(&valopt), &lon) < 0) {
                        setLastSocketErrNo(errno);
                        addCommunicationLogItem(strerror(errno));
                        return SOCKET_INIT_SESSION_GET_SOCKET_OPTIONS_ERROR;
                    }
                    // Check the value returned...
                    if (valopt) {
                        setLastSocketErrNo(valopt);
                        addCommunicationLogItem(strerror(valopt));
                        return SOCKET_INIT_SESSION_DELAYED_CONNECTION_ERROR;
                    }
                    break;
                } else {
                    return SOCKET_INIT_SESSION_CONNECT_TIMEOUT;
                }
            } while (1);
        } else {
            setLastSocketErrNo(errno);
            addCommunicationLogItem(strerror(errno));
            return SOCKET_INIT_SESSION_CONNECT_ERROR;
        }
    }
    // Set to blocking mode again...
    return setSocketToBlockingPOSIX();
}

int SMTPClientBase::setSocketToNonBlockingPOSIX() {
    int arg;
    if ((arg = fcntl(mSock, F_GETFL, NULL)) < 0) {
        setLastSocketErrNo(errno);
        addCommunicationLogItem(strerror(errno));
    }
    arg |= O_NONBLOCK;
    if (fcntl(mSock, F_SETFL, arg) < 0) {
        setLastSocketErrNo(errno);
        addCommunicationLogItem(strerror(errno));
        return SOCKET_INIT_SESSION_FCNTL_SET_ERROR;
    }
    return 0;
}

int SMTPClientBase::setSocketToBlockingPOSIX() {
    int arg;
    if ((arg = fcntl(mSock, F_GETFL, NULL)) < 0) {
        setLastSocketErrNo(errno);
        addCommunicationLogItem(strerror(errno));
        return SOCKET_INIT_SESSION_FCNTL_GET_ERROR;
    }
    arg &= (~O_NONBLOCK);
    if (fcntl(mSock, F_SETFL, arg) < 0) {
        setLastSocketErrNo(errno);
        addCommunicationLogItem(strerror(errno));
        return SOCKET_INIT_SESSION_FCNTL_SET_ERROR;
    }
    return 0;
}
#endif

int SMTPClientBase::sendServerIdentification() {
    std::string ehlo { "ehlo localhost\r\n" };
    addCommunicationLogItem(ehlo.c_str());
    return sendRawCommand(ehlo.c_str(),
            SOCKET_INIT_CLIENT_SEND_EHLO_ERROR,
            SOCKET_INIT_CLIENT_SEND_EHLO_TIMEOUT);
}

int SMTPClientBase::checkServerGreetings() {
    char outbuf[SERVERRESPONSE_BUFFER_LENGTH];
    unsigned int waitTime = 0;
    ssize_t bytes_received = 0;
    while ((bytes_received = recv(mSock, outbuf, SERVERRESPONSE_BUFFER_LENGTH, 0)) <= 0
            && waitTime < mCommandTimeOut) {
        sleep(1);
        waitTime += 1;
    }
    if (waitTime < mCommandTimeOut) {
        outbuf[bytes_received-1] = '\0';
        addCommunicationLogItem(outbuf, "s");
        int status_code = extractReturnCode(outbuf);
        if (status_code == STATUS_CODE_SERVICE_READY) {
            addCommunicationLogItem("Connected!");
        }
        return status_code;
    }
    return SOCKET_INIT_SESSION_CONNECT_TIMEOUT;
}

int SMTPClientBase::sendRawCommand(const char *pCommand, int pErrorCode) {
#ifdef _WIN32
    size_t pCommandSize = strlen(pCommand);
    if (static_cast<intmax_t>(pCommandSize) > (std::numeric_limits<int>::max)()) {
        return pErrorCode;
    }
    int commandSize = static_cast<int>(pCommandSize);
#else
    size_t commandSize = strlen(pCommand);
#endif
    if (send(mSock, pCommand, commandSize, 0) == -1) {
        setLastSocketErrNo(errno);
        cleanup();
        return pErrorCode;
    }
    return 0;
}

int SMTPClientBase::sendRawCommand(const char *pCommand, int pErrorCode, int pTimeoutCode) {
    char outbuf[SERVERRESPONSE_BUFFER_LENGTH];
    unsigned int waitTime {0};
    ssize_t bytes_received {0};
    if (sendRawCommand(pCommand, pErrorCode) != 0) {
        return pErrorCode;
    }

    while ((bytes_received = recv(mSock, outbuf, SERVERRESPONSE_BUFFER_LENGTH, 0)) <= 0 && waitTime < mCommandTimeOut) {
        sleep(1);
        waitTime += 1;
    }
    if (waitTime < mCommandTimeOut) {
        outbuf[bytes_received-1] = '\0';
        setLastServerResponse(outbuf);
        addCommunicationLogItem(outbuf, "s");
        return extractReturnCode(outbuf);
    }

    cleanup();
    return pTimeoutCode;
}

void SMTPClientBase::setLastServerResponse(const char *pResponse) {
    delete[] mLastServerResponse;
    size_t response_len = strlen(pResponse);
    mLastServerResponse = new char[response_len + 1];
    strncpy(mLastServerResponse, pResponse, response_len);
    mLastServerResponse[response_len] = '\0';
}

int SMTPClientBase::authenticateClient() {
    if (mCredential != nullptr) {
        if (mAuthOptions->Plain) {
            return authenticateWithMethodPlain();
        }
        if (mAuthOptions->Login) {
            return authenticateWithMethodLogin();
        }
        return CLIENT_AUTHENTICATION_METHOD_NOTSUPPORTED;
    }
    return CLIENT_AUTHENTICATE_NONEED;
}

int SMTPClientBase::authenticateWithMethodPlain() {
    addCommunicationLogItem("AUTH PLAIN ***************\r\n");
    std::stringstream ss_credentials;
    // Format : \0username\0password
    ss_credentials << '\0' << mCredential->getUsername() << '\0' << mCredential->getPassword();
    std::string str_credentials = ss_credentials.str();
    std::stringstream ss;
    ss << "AUTH PLAIN "
        << Base64::Encode(reinterpret_cast<const unsigned char*>(str_credentials.c_str()),
                strlen(mCredential->getUsername()) + strlen(mCredential->getPassword()) + 2)  // + 2 for the two null characters
        << "\r\n";
    return (*this.*sendCommandWithFeedbackPtr)(ss.str().c_str(), CLIENT_AUTHENTICATE_ERROR, CLIENT_AUTHENTICATE_TIMEOUT);
}

int SMTPClientBase::authenticateWithMethodLogin() {
    addCommunicationLogItem("AUTH LOGIN ***************\r\n");
    int login_return_code = (*this.*sendCommandWithFeedbackPtr)("AUTH LOGIN\r\n",
            CLIENT_AUTHENTICATE_ERROR,
            CLIENT_AUTHENTICATE_TIMEOUT);
    if (login_return_code != STATUS_CODE_SERVER_CHALLENGE) {
        return CLIENT_AUTHENTICATE_ERROR;
    }

    std::string encoded_username { Base64::Encode(reinterpret_cast<const unsigned char*>(mCredential->getUsername()),
            strlen(mCredential->getUsername())) };
    std::stringstream ss_username;
    ss_username << encoded_username << "\r\n";
    int username_return_code = (*this.*sendCommandWithFeedbackPtr)(ss_username.str().c_str(),
            CLIENT_AUTHENTICATE_ERROR,
            CLIENT_AUTHENTICATE_TIMEOUT);
    if (username_return_code != STATUS_CODE_SERVER_CHALLENGE) {
        return CLIENT_AUTHENTICATE_ERROR;
    }
    std::string encoded_password { Base64::Encode(reinterpret_cast<const unsigned char*>(mCredential->getPassword()),
            strlen(mCredential->getPassword())) };
    std::stringstream ss_password;
    ss_password << encoded_password << "\r\n";
    return (*this.*sendCommandWithFeedbackPtr)(ss_password.str().c_str(), CLIENT_AUTHENTICATE_ERROR, CLIENT_AUTHENTICATE_TIMEOUT);
}

int SMTPClientBase::setMailRecipients(const Message &pMsg) {

    const int SENDER_OK { 250 };
    const int RECIPIENT_OK { 250 };
    std::string mailFormat = "MAIL FROM: <"s + pMsg.getFrom().getEmailAddress() + ">\r\n";

    addCommunicationLogItem(mailFormat.c_str());
    int mail_from_ret_code = (*this.*sendCommandWithFeedbackPtr)(mailFormat.c_str(), CLIENT_SENDMAIL_MAILFROM_ERROR, CLIENT_SENDMAIL_MAILFROM_TIMEOUT);

    // If no compatible format were found
    if (mail_from_ret_code != SENDER_OK) {
        return mail_from_ret_code;
    }

    // Send command for the recipients
    std::vector<std::pair<MessageAddress **, size_t>> recipients {
        std::pair<MessageAddress **, size_t>(pMsg.getTo(), pMsg.getToCount()),
            std::pair<MessageAddress **, size_t>(pMsg.getCc(), pMsg.getCcCount()),
            std::pair<MessageAddress **, size_t>(pMsg.getBcc(), pMsg.getBccCount())
    };
    for (const auto &item : recipients) {
        if (item.first != nullptr) {
            int rcpt_to_ret_code = addMailRecipients(item.first, item.second, RECIPIENT_OK);
            if (rcpt_to_ret_code != RECIPIENT_OK) {
                return rcpt_to_ret_code;
            }
        }
    }
    return 0;
}

int SMTPClientBase::addMailRecipients(jed_utils::MessageAddress **list, size_t count, const int RECIPIENT_OK) {
    int rcpt_to_ret_code = RECIPIENT_OK;
    std::for_each(list, list + count, [this, &rcpt_to_ret_code, &RECIPIENT_OK](MessageAddress *address) {
            std::stringstream ss_rcpt_to;
            ss_rcpt_to << "RCPT TO: <"s << address->getEmailAddress() << ">\r\n"s;
            addCommunicationLogItem(ss_rcpt_to.str().c_str());
            int ret_code = (*this.*sendCommandWithFeedbackPtr)(ss_rcpt_to.str().c_str(), CLIENT_SENDMAIL_RCPTTO_ERROR, CLIENT_SENDMAIL_RCPTTO_TIMEOUT);
            if (ret_code != RECIPIENT_OK) {
            rcpt_to_ret_code = ret_code;
            }
            });
    return rcpt_to_ret_code;
}

int SMTPClientBase::setMailHeaders(const Message &pMsg) {
    // Data section
    std::string data_cmd = "DATA\r\n";
    addCommunicationLogItem(data_cmd.c_str());
    int data_ret_code = (*this.*sendCommandWithFeedbackPtr)(data_cmd.c_str(), CLIENT_SENDMAIL_DATA_ERROR, CLIENT_SENDMAIL_DATA_TIMEOUT);
    if (data_ret_code != STATUS_CODE_START_MAIL_INPUT) {
        return data_ret_code;
    }

    // Mail headers
    // From
    std::string from_header =  "\"" + std::string(pMsg.getFrom().getDisplayName()) + "\" <" + pMsg.getFrom().getEmailAddress() + ">";
    int header_from_ret_code = addMailHeader("From", from_header.c_str() , CLIENT_SENDMAIL_HEADERFROM_ERROR);
    if (header_from_ret_code != 0) {
        return header_from_ret_code;
    }

    // To and Cc.
    // Note : Bcc are not included in the header
    std::vector<std::tuple<MessageAddress **, size_t, const char *>> recipients {
        std::tuple<MessageAddress **, size_t, const char *>(pMsg.getTo(), pMsg.getToCount(), "To"),
            std::tuple<MessageAddress **, size_t, const char *>(pMsg.getCc(), pMsg.getCcCount(), "Cc")
    };
    for (const auto &item : recipients) {
        MessageAddress **list = std::get<0>(item);
        size_t count = std::get<1>(item);
        const char *field = std::get<2>(item);
        if (list != nullptr) {
            std::for_each(list, list + count, [this, &field](MessageAddress *address) {
                    return addMailHeader(field, address->getEmailAddress(), CLIENT_SENDMAIL_HEADERTOANDCC_ERROR);
                    });
        }
    }

    // Subject
    std::stringstream ss_header_subject;
    ss_header_subject << "Subject: " << pMsg.getSubject() << "\r\n";
    addCommunicationLogItem(ss_header_subject.str().c_str());
    int header_subject_ret_code = (*this.*sendCommandPtr)(ss_header_subject.str().c_str(), CLIENT_SENDMAIL_HEADERSUBJECT_ERROR);
    if (header_subject_ret_code != 0) {
        return header_subject_ret_code;
    }

    // Content-Type
    std::string content_type { "Content-Type: multipart/mixed; boundary=sep\r\n\r\n" };
    addCommunicationLogItem(content_type.c_str());
    int header_content_type_ret_code = (*this.*sendCommandPtr)(content_type.c_str(), CLIENT_SENDMAIL_HEADERCONTENTTYPE_ERROR);
    if (header_content_type_ret_code != 0) {
        return header_content_type_ret_code;
    }

    return 0;
}

int SMTPClientBase::addMailHeader(const char *field, const char *value, int pErrorCode) {
    std::stringstream ss_header_item;
    ss_header_item << field << ": " << value << "\r\n";
    addCommunicationLogItem(ss_header_item.str().c_str());
    return (*this.*sendCommandPtr)(ss_header_item.str().c_str(), pErrorCode);
}

int SMTPClientBase::setMailBody(const Message &pMsg) {
    // Body part
    std::ostringstream body_ss;
    body_ss << "--sep\r\nContent-Type: " << pMsg.getMimeType() << "; charset=UTF-8\r\n\r\n" << pMsg.getBody() << "\r\n";
    std::string body_real = body_ss.str();
    addCommunicationLogItem(body_real.c_str());

    // If there's attachments, prepare the attachments text content
    Attachment** arr_attachment = pMsg.getAttachments();

    std::vector<Attachment*> vect_attachment(arr_attachment, arr_attachment + pMsg.getAttachmentsCount());
    if (pMsg.getAttachmentsCount() > 0) {
        std::string attachments_text = createAttachmentsText(vect_attachment);
        body_real += attachments_text;
    }

    const size_t CHUNK_MAXLENGTH = 512;
    if (body_real.length() > CHUNK_MAXLENGTH) {
        // Split into chunk
        for (size_t index_start = 0; index_start < body_real.length(); index_start += CHUNK_MAXLENGTH) {
            size_t length = CHUNK_MAXLENGTH;
            if (index_start + CHUNK_MAXLENGTH > body_real.length() - 1) {
                length = body_real.length() - index_start;
            }
            int body_part_ret_code = (*this.*sendCommandPtr)(body_real.substr(index_start, length).c_str(), CLIENT_SENDMAIL_BODYPART_ERROR);
            if (body_part_ret_code != 0) {
                return body_part_ret_code;
            }
        }
    } else {
        int body_ret_code = (*this.*sendCommandPtr)(body_real.c_str(), CLIENT_SENDMAIL_BODY_ERROR);
        if (body_ret_code != 0) {
            return body_ret_code;
        }
    }

    // End of data
    std::string end_data_command { "\r\n.\r\n" };
    addCommunicationLogItem(end_data_command.c_str());
    int end_data_ret_code = (*this.*sendCommandWithFeedbackPtr)(end_data_command.c_str(), CLIENT_SENDMAIL_END_DATA_ERROR, CLIENT_SENDMAIL_END_DATA_TIMEOUT);
    if (end_data_ret_code != STATUS_CODE_REQUESTED_MAIL_ACTION_OK_OR_COMPLETED) {
        return end_data_ret_code;
    }

    std::string quit_command { "QUIT\r\n" };
    addCommunicationLogItem(quit_command.c_str());
    int quit_ret_code = (*this.*sendCommandPtr)(quit_command.c_str(), CLIENT_SENDMAIL_QUIT_ERROR);
    if (quit_ret_code != 0) {
        return quit_ret_code;
    }
    return 0;
}

void SMTPClientBase::addCommunicationLogItem(const char *pItem, const char *pPrefix) {
    std::string item { pItem };
    if (strcmp(pPrefix, "c") == 0) {
        /* Replace the \ by \\ */
        const std::string FROM { "\r\n" };
        const std::string TO { "\\r\\n" };
        size_t start_pos = 0;
        while ((start_pos = item.find(FROM, start_pos)) != std::string::npos) {
            item.replace(start_pos, FROM.length(), TO);
            start_pos += TO.length();  // Handles case where 'to' is a substring of 'from'
        }
    }
    const std::string ENDOFLINE { "\n" };
    const std::string SEPARATOR { ": " };
    size_t currentLogSize = strlen(mCommunicationLog);
    size_t appendSize = ENDOFLINE.length() + strlen(pPrefix) + SEPARATOR.length() + item.length() + 1;
    if (mCommunicationLogSize - currentLogSize <= appendSize) {
        size_t newSize = currentLogSize + appendSize + INITIAL_COMM_LOG_LENGTH;
        char *newBuffer = new char[newSize];
        std::strncpy(newBuffer, mCommunicationLog, currentLogSize);
        newBuffer[currentLogSize] = '\0';
        mCommunicationLogSize = newSize;
        char *tmpBuffer = mCommunicationLog;
        mCommunicationLog = newBuffer;
        delete [] tmpBuffer;
    }
    strncat(mCommunicationLog, ENDOFLINE.c_str(), mCommunicationLogSize-1);
    strncat(mCommunicationLog, pPrefix, mCommunicationLogSize-1);
    strncat(mCommunicationLog, SEPARATOR.c_str(), mCommunicationLogSize-1);
    strncat(mCommunicationLog, item.c_str(), mCommunicationLogSize-1);
    mCommunicationLog[mCommunicationLogSize-1] = '\0';
}

std::string SMTPClientBase::createAttachmentsText(const std::vector<Attachment*> &pAttachments) {
    std::string retval;
    for (const auto &item : pAttachments) {
        retval += "\r\n--sep\r\n";
        retval += "Content-Type: " + std::string(item->getMimeType()) + "; file=\"" + std::string(item->getName()) + "\"\r\n";
        retval += "Content-Disposition: Inline; filename=\"" + std::string(item->getName()) + "\"\r\n";
        retval += "Content-Transfer-Encoding: base64\r\n\r\n";
        retval += std::string((item->getBase64EncodedFile() != nullptr ? item->getBase64EncodedFile() : ""));
    }
    retval += "\r\n--sep--";
    return retval;
}

int SMTPClientBase::extractReturnCode(const char *pOutput) {
    if (pOutput != nullptr && strlen(pOutput) >= 3) {
        std::string code_str { pOutput };
        try {
            return std::stoi(code_str.substr(0, 3));
        }
        catch (std::exception const &) {
            return -1;
        }
    }
    return -1;
}

ServerAuthOptions *SMTPClientBase::extractAuthenticationOptions(const char *pEhloOutput) {
    ServerAuthOptions *retVal = nullptr;
    const std::string AUTH_LINE_PREFIX = "250-AUTH";
    if (pEhloOutput == nullptr) {
        return retVal;
    }
    const std::string DELIMITER { "\r\n" };
    std::string ehlo_output { pEhloOutput };
    size_t ehlo_character_index { 0 };
    while ((ehlo_character_index = ehlo_output.find(DELIMITER)) != std::string::npos) {
        std::string line { ehlo_output.substr(0, ehlo_character_index)};
        // Find the line that begin with 250-AUTH
        if (line.substr(0, AUTH_LINE_PREFIX.length()) == AUTH_LINE_PREFIX) {
            retVal = new ServerAuthOptions();
            // Find each options
            std::vector<std::string> options;
            size_t line_character_index { 0 };
            while ((line_character_index = line.find(' ')) != std::string::npos) {
                std::string option { line.substr(0, line_character_index)};
                options.push_back(option);
                line.erase(0, line_character_index + 1);
            }
            options.push_back(line);
            // Try to match the string options with the ServerAuthOptions struct attributes
            for_each(options.begin(), options.end(), [&retVal](const std::string &option) {
                if (option == "PLAIN") {
                    retVal->Plain = true;
                } else if (option == "LOGIN") {
                    retVal->Login = true;
                } else if (option == "XOAUTH2") {
                    retVal->XOAuth2 = true;
                } else if (option == "PLAIN-CLIENTTOKEN") {
                    retVal->Plain_ClientToken = true;
                } else if (option == "OAUTHBEARER") {
                    retVal->OAuthBearer = true;
                } else if (option == "XOAUTH") {
                    retVal->XOAuth = true;
                }
                });
            break;
        }
        ehlo_output.erase(0, ehlo_character_index + DELIMITER.length());
    }
    return retVal;
}
