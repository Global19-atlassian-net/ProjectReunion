// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include <pch.h>
#include <shlwapi.h>
#include <AppLifecycle.h>
#include <AppLifecycle.g.cpp>

#include "LaunchActivatedEventArgs.h"
#include "ProtocolActivatedEventArgs.h"
#include "FileActivatedEventArgs.h"
#include "Association.h"

namespace winrt::Microsoft::ProjectReunion::implementation
{
    using Windows::ApplicationModel::Activation::IActivatedEventArgs;

    std::tuple<std::wstring, std::wstring> ParseCommandLine(std::wstring commandLine)
    {
        auto argsStart = commandLine.rfind(L"----");
        if (argsStart == std::wstring::npos)
        {
            return {L"", L""};
        }

        argsStart += 4;

        // We explicitly use find_first_of here, so that the resulting data may contain : as a valid character.
        auto argsEnd = commandLine.find_first_of(L":", argsStart);
        if (argsEnd == std::wstring::npos)
        {
            return {L"", L""};
        }

        if (argsStart > argsEnd)
        {
            throw std::overflow_error("commandLine");
        }

        auto argsLength = argsEnd - argsStart;
        auto dataStart = argsEnd + 1;

        return {commandLine.substr(argsStart, argsLength), commandLine.substr(dataStart)};
    }

    std::string WideCharToMultiByte(std::wstring in)
    {
        auto len = ::WideCharToMultiByte(CP_ACP, 0, in.c_str(), static_cast<DWORD>(in.size()), nullptr, 0, nullptr, nullptr);
        THROW_LAST_ERROR_IF(len == 0);

        std::vector<char> out(len);
        THROW_LAST_ERROR_IF(0 == ::WideCharToMultiByte(CP_ACP, 0, in.c_str(),
            static_cast<DWORD>(in.size()), out.data(), len, nullptr, nullptr));
        return std::string(out.begin(), out.end());
    }

    std::vector<BYTE> UrlUnescape(std::wstring const& url)
    {
        auto data = WideCharToMultiByte(url);

        std::vector<BYTE> unescaped(url.size());
        DWORD length = static_cast<DWORD>(unescaped.size());

        HRESULT unescapedResult = ::UrlUnescapeA(const_cast<PSTR>(data.c_str()),
            reinterpret_cast<PSTR>(unescaped.data()), &length, 0);
        if (unescapedResult == E_POINTER)
        {
            unescaped.resize(length);
            unescapedResult = ::UrlUnescapeA(const_cast<PSTR>(data.c_str()),
                reinterpret_cast<PSTR>(unescaped.data()), &length, 0);
        }
        THROW_IF_FAILED(unescapedResult);

        return unescaped;
    }

    IActivatedEventArgs UnmarshalArgs(std::wstring const& encodedStream)
    {
        com_ptr<::IStream> stream;
        THROW_IF_FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));

        auto decodedStream = UrlUnescape(encodedStream);

        ULONG bytesWritten = 0;
        THROW_IF_FAILED(stream->Write(decodedStream.data(), static_cast<DWORD>(decodedStream.size()), &bytesWritten));

        const LARGE_INTEGER headOffset{};
        THROW_IF_FAILED(stream->Seek(headOffset, STREAM_SEEK_SET, nullptr));

        com_ptr<IUnknown> unk;
        THROW_IF_FAILED(::CoUnmarshalInterface(stream.get(), guid_of<IActivatedEventArgs>(), unk.put_void()));

        return unk.try_as<IActivatedEventArgs>();
    }

    IActivatedEventArgs AppLifecycle::GetActivatedEventArgs()
    {
        if (HasIdentity())
        {
            return Windows::ApplicationModel::AppInstance::GetActivatedEventArgs();
        }
        else
        {
            // Generate IActivatedEventArgs for non-Packaged applications.
            std::wstring contractId;
            std::wstring contractData;
            auto commandLine = std::wstring(GetCommandLine());
            std::tie(contractId, contractData) = ParseCommandLine(commandLine);

            if (!contractId.empty())
            {
                if (contractId == c_protocolArgumentString)
                {
                    auto args = make<ProtocolActivatedEventArgs>(contractData);
                    auto uri = args.as<IProtocolActivatedEventArgs>().Uri();
                    if (uri.SchemeName() == L"ms-encodedlaunch")
                    {
                        __debugbreak();
                        std::wstring encodedArgStream = uri.QueryParsed().GetFirstValueByName(L"MarshaledObject").c_str();
                        return UnmarshalArgs(encodedArgStream);
                    }

                    return args;
                }
                else if (contractId == c_fileArgumentString)
                {
                    return make<FileActivatedEventArgs>(contractData);
                }
            }

            return make<LaunchActivatedEventArgs>(commandLine);
        }
    }
}
