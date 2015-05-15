///////////////////////////////////////////////////////////////////////////////
//
// Copyright 2015 Intel Mobile Communications GmbH All Rights Reserved.
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
//
//
///////////////////////////////////////////////////////////////////////////////

/// @file ra_xmpp_over_ccfxmpp.cpp

#ifdef _WIN32
#include <SDKDDKVer.h>
#endif


#include <string>
#include <iostream>
#include <map>


#ifdef ENABLE_LIBSTROPHE
#include <xmpp/xmppstrophe.h>
#else
#include <connect/tcpclient.h>
#include <xmpp/xmppclient.h>
#endif

#include <xmpp/xmppconfig.h>
#include <xmpp/sasl.h>

#include <connect/proxy.h>

#include "ra_xmpp.h"

using namespace std;
using namespace Iotivity;


struct static_init_test
{
        static_init_test()
        {
            m_init_has_run[0] = 'R'; m_init_has_run[1] = 'U';
            m_init_has_run[2] = 'N'; m_init_has_run[3] = '\0';
        }

        // Test for C++ static init. This is intended to fail gracefully even if the static
        // initialization did not run.
        bool has_initalized() const
        {
            return m_init_has_run[0] == 'R' && m_init_has_run[1] == 'U' &&
                   m_init_has_run[2] == 'N';
        }
    private:
        volatile char m_init_has_run[4];
};

static static_init_test s_init_test;


using namespace Iotivity::Xmpp;

struct UserIdentity
{

};



struct ContextWrapper
{
        ContextWrapper()
        {}

        ~ContextWrapper()
        {}

        void connect(void *const handle, const string &host, const string &port,
                     const ProxyConfig &proxy, const string &userName,
                     const SecureBuffer &password, const string &userJID,
                     const string &xmppDomain, InBandRegister inbandRegistrationAction,
                     XMPP_LIB_(connection_callback_t) callback)
        {
#ifdef ENABLE_LIBSTROPHE
            auto xmlConnection = make_shared<XmppStropheConnection>(host, port);
#else
            auto remoteTcp = make_shared<TcpConnection>(host, port, proxy);

            auto xmlConnection = make_shared<XmppConnection>(
                                     static_pointer_cast<IStreamConnection>(remoteTcp));
#endif // ENABLE_LIBSTROPHE

            XmppConfig config(JabberID(userJID), xmppDomain);
            config.requireTLSNegotiation();

            config.setSaslConfig("SCRAM-SHA-1", SaslScramSha1::Params::create(userName, password));
            config.setSaslConfig("PLAIN", SaslPlain::Params::create(userName, password));

            m_client = XmppClient::create();

            auto createdFunc =
                [handle, callback](XmppStreamCreatedEvent & e)
            {
                if (e.result().succeeded())
                {
                    const void *streamHandle = e.stream().get();
                    {
                        lock_guard<recursive_mutex> lock(mutex());
                        s_streamsByHandle[streamHandle] = e.stream();
                    }

                    auto streamConnectedFunc =
                        [streamHandle, callback](XmppConnectedEvent & e)
                    {
                        if (callback.on_connected)
                        {
                            callback.on_connected(callback.param, translateError(e.result()),
                                                  streamHandle);
                        }
                    };
                    using StreamConnectedFunc = NotifySyncFunc<XmppConnectedEvent,
                          decltype(streamConnectedFunc)>;
                    e.stream()->onConnected() += make_shared<StreamConnectedFunc>(
                                                     streamConnectedFunc);

                    auto streamClosedFunc =
                        [streamHandle, callback](XmppClosedEvent & e)
                    {
                        if (callback.on_disconnected)
                        {
                            callback.on_disconnected(callback.param,
                                                     translateError(e.result()),
                                                     streamHandle);
                        }

                        {
                            lock_guard<recursive_mutex> lock(mutex());
                            s_streamsByHandle.erase(streamHandle);
                        }
                    };
                    using StreamClosedFunc = NotifySyncFunc<XmppClosedEvent,
                          decltype(streamClosedFunc)>;
                    e.stream()->onClosed() += make_shared<StreamClosedFunc>(streamClosedFunc);
                }
                else
                {
                    XMPP_LIB_(error_code_t) errorCode = translateError(e.result());
                    if (callback.on_connected && isValidWrapper(handle))
                    {
                        callback.on_connected(callback.param, errorCode, handle);
                    }
                }
            };

            using StreamCreatedFunc = NotifySyncFunc<XmppStreamCreatedEvent, decltype(createdFunc)>;
            m_client->onStreamCreated() += make_shared<StreamCreatedFunc>(createdFunc);

            m_client->initiateXMPP(config, xmlConnection);
        }

        static shared_ptr<IXmppStream> streamByHandle(XMPP_LIB_(connection_handle_t) connection)
        {
            lock_guard<recursive_mutex> lock(mutex());
            auto f = s_streamsByHandle.find(connection);
            return f != s_streamsByHandle.end() ? f->second.lock() : shared_ptr<IXmppStream>();
        }

        static XMPP_LIB_(error_code_t) translateError(const connect_error &ce)
        {
            XMPP_LIB_(error_code_t) errorCode = XMPP_ERR_FAIL;
            if (ce.succeeded())
            {
                errorCode = XMPP_ERR_OK;
            }
            else if (ce.errorType() == connect_error::etConnectError())
            {
                switch (ce.errorCode())
                {
                    case connect_error::ecSuccess:
                        errorCode = XMPP_ERR_OK;
                        break;
                    case connect_error::ecTLSNegotiationInProgress:
                    case connect_error::ecStreamResourceNotBound:
                        errorCode = XMPP_ERR_STREAM_NOT_NEGOTIATED;
                        break;
                    case connect_error::ecServerClosedStream:
                    case connect_error::ecSocketClosed:
                        errorCode = XMPP_ERR_SERVER_DISCONNECTED;
                        break;
                    case connect_error::ecNotSupported:
                        errorCode =  XMPP_ERR_FEATURE_NOT_SUPPORTED;
                        break;
                    case connect_error::ecXMLParserError:
                    case connect_error::ecUnknownSID:
                    case connect_error::ecSIDReused:
                    case connect_error::ecQueryIDAlreadySubmitted:
                    case connect_error::ecAttemptToRestartBoundStream:
                        errorCode = XMPP_ERR_INTERNAL_ERROR;
                        break;
                    case connect_error::ecWaitMissing:
                    case connect_error::ecRequestsMissing:
                        errorCode = XMPP_ERR_BOSH_ERROR;
                        break;
                    case connect_error::ecUnableToStartSession:
                    case connect_error::ecInvalidStream:
                    case connect_error::ecUnableToBindUser:
                        errorCode = XMPP_ERR_STREAM_NOT_NEGOTIATED;
                        break;
                    case connect_error::ecInvalidPort:
                        errorCode = XMPP_ERR_HOST_CONNECTION_FAILED;
                        break;
                    case connect_error::ecHostNameTooLongForSOCKS5:
                    case connect_error::ecUnknownSOCKS5AddressType:
                    case connect_error::ecUserNameTooLongForSOCKS5:
                    case connect_error::ecPasswordTooLongForSOCKS5:
                    case connect_error::ecSOCKS5InvalidUserNameOrPassword:
                    case connect_error::ecProxyTypeNotSupported:
                        errorCode = XMPP_ERR_PROXY_CONNECT_ERROR;
                        break;
                    case connect_error::ecTlsNegotationFailure:
                        errorCode = XMPP_ERR_TLS_NEGOTIATION_FAILED;
                        break;
                    case connect_error::ecSaslNegotationFailure:
                    case connect_error::ecSaslNegotationAborted:
                    case connect_error::ecNoSaslMechanism:
                    case connect_error::ecInsecureSaslOverInsecureStream:
                    case connect_error::ecErrorEncodingNonce:
                        errorCode = XMPP_ERR_SASL_NEGOTIATION_FAILED;
                        break;
                    case connect_error::ecRegistrationAlreadyRunning:
                    case connect_error::ecInvalidRegistration:
                        errorCode = XMPP_ERR_INBAND_REGISTRATION_FAILURE;
                        break;
                    case connect_error::ecRequestFailed:
                        errorCode = XMPP_ERR_REQUEST_ERROR_RESPONSE;
                        break;
                    case connect_error::ecExtensionInShutdown:
                        errorCode = XMPP_ERR_STREAM_CLOSING_NOT_AVAILABLE;
                        break;
                    case connect_error::ecSocketConnectError:
                        errorCode = XMPP_ERR_CONNECT_ERROR;
                        break;
                    case connect_error::ecStanzaTranslationError:
                    case connect_error::ecStanzaTooLong:
                        errorCode = XMPP_ERR_INVALID_SERVER_STANZA;
                        break;
                    case connect_error::ecStreamInShutdown:
                    default:
                        break;
                }
            }
            else if (ce.errorType() == connect_error::etCurlError())
            {
                errorCode = XMPP_ERR_BOSH_ERROR;
            }
            else if (ce.errorType() == connect_error::etHttpError())
            {
                errorCode = XMPP_ERR_BOSH_ERROR;
            }
            else if (ce.errorType() == connect_error::etSOCKS5Error())
            {
                errorCode = XMPP_ERR_PROXY_CONNECT_ERROR;
            }
            else if (ce.errorType() == connect_error::etASIOError())
            {
                // Fold any ASIO errors (c++ client only) into a generic connect error.
                errorCode = XMPP_ERR_CONNECT_ERROR;
            }
            return errorCode;
        }


        static void addWrapper(const void *const wrapper)
        {
            lock_guard<recursive_mutex> lock(mutex());
            s_wrappers.insert(wrapper);
        }

        static bool isValidWrapper(const void *const wrapper)
        {
            lock_guard<recursive_mutex> lock(mutex());
            return s_wrappers.find(wrapper) != s_wrappers.end();
        }

        static void removeWrapper(const void *const wrapper)
        {
            lock_guard<recursive_mutex> lock(mutex());
            s_wrappers.erase(wrapper);
        }

    protected:
        static recursive_mutex &mutex()
        {
            static recursive_mutex s_mutex;
            return s_mutex;
        }

    private:
        shared_ptr<XmppClient> m_client{};

        static set<const void *> s_wrappers;
        using StreamHandleMap = map<const void *, weak_ptr<IXmppStream>>;
        static StreamHandleMap s_streamsByHandle;
};

set<const void *> ContextWrapper::s_wrappers;
ContextWrapper::StreamHandleMap ContextWrapper::s_streamsByHandle;


extern "C"
{
///////////////////////////////////////////////////////////////////////////////////////////////////
// C Abstraction Interface
///////////////////////////////////////////////////////////////////////////////////////////////////
    void *const xmpp_wrapper_create_wrapper(void)
    {
        // If static-init did not run, we cannot continue. We also can't safely use
        // iostream, so we'll avoid logging the failure here. This might happen if we are executed from
        // within a C-only exe that never executes C++ static init. That is not supported. Note
        // that if you decide to support a log message here, please do not use printf; this library
        // has avoided the overhead of adding printf.
        if (!s_init_test.has_initalized())
        {
            return nullptr;
        }
        try
        {
            ContextWrapper *wrapper = new ContextWrapper;
            ContextWrapper::addWrapper(wrapper);
            return wrapper;
        }
        catch (...) {}
        return nullptr;
    }

    void xmpp_wrapper_destroy_wrapper(void *const handle)
    {
        try
        {
            if (handle)
            {
                ContextWrapper *wrapper = reinterpret_cast<ContextWrapper *>(handle);
                if (ContextWrapper::isValidWrapper(wrapper))
                {
                    ContextWrapper::removeWrapper(wrapper);
                    delete wrapper;
                }
            }
        }
        catch (...) {}
    }

    XMPP_LIB_(error_code_t) xmpp_wrapper_connect(void *const handle,
            const XMPP_LIB_(host_t) * const host,
            const XMPP_LIB_(identity_t) * const identity,
            const XMPP_LIB_(proxy_t) * const proxy,
            XMPP_LIB_(connection_callback_t) callback)
    {
        if (!handle)
        {
            return XMPP_ERR_INVALID_HANDLE;
        }
        if (!host || !identity)
        {
            return XMPP_ERR_INVALID_PARAMETER;
        }
        try
        {
            ContextWrapper *wrapper = reinterpret_cast<ContextWrapper *>(handle);

            if (!ContextWrapper::isValidWrapper(wrapper))
            {
                return XMPP_ERR_INVALID_HANDLE;
            }

            string userName;
            if (identity->user_name)
            {
                userName = identity->user_name;
            }

            SecureBuffer password;
            if (identity->password)
            {
                string passStr = identity->password;
                password.setBuffer(passStr.c_str(), passStr.size());
            }

            string userJID;
            if (identity->user_jid)
            {
                userJID = identity->user_jid;
            }

            string hostStr;
            if (host->host)
            {
                hostStr = host->host;
            }

            string xmppDomain;
            if (host->xmpp_domain)
            {
                xmppDomain = host->xmpp_domain;
            }
            else
            {
                xmppDomain = hostStr;
            }

            auto portStr = to_string(host->port);
            auto proxyType = ProxyConfig::ProxyType::ProxyUndefined;
            if (proxy)
            {
                switch (proxy->proxy_type)
                {
                    case XMPP_PROXY_DIRECT_CONNECT:
                        proxyType = ProxyConfig::ProxyType::ProxyUndefined;
                        break;
                    case XMPP_PROXY_SOCKS5:
                        proxyType = ProxyConfig::ProxyType::ProxySOCKS5;
                        break;
                }
            }
            auto &&proxyConfig = proxy ? ProxyConfig(proxy->proxy_host, to_string(proxy->proxy_port),
                                 proxyType) :
                                 ProxyConfig();

            wrapper->connect(handle, hostStr, portStr, proxyConfig, userName, password,
                             userJID, xmppDomain, identity->inband_registration, callback);

            return XMPP_ERR_OK;
        }
        catch (const connect_error &ce)
        {
            return ContextWrapper::translateError(ce);
        }
        catch (...) {}
        return XMPP_ERR_INTERNAL_ERROR;
    }


    XMPP_LIB_(error_code_t) xmpp_wrapper_disconnect(XMPP_LIB_(connection_handle_t) connection)
    {
        if (!connection)
        {
            return XMPP_ERR_INVALID_HANDLE;
        }
        try
        {
            shared_ptr<IXmppStream> stream = ContextWrapper::streamByHandle(connection);
            if (!stream)
            {
                return XMPP_ERR_INVALID_HANDLE;
            }
            stream->close();
            return XMPP_ERR_OK;
        }
        catch (const connect_error &ce)
        {
            return ContextWrapper::translateError(ce);
        }
        catch (...) {}

        return XMPP_ERR_INTERNAL_ERROR;
    }

} // extern "C"


