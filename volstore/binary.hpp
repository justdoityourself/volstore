/* Copyright (C) 2020 D8DATAWORKS - All Rights Reserved */

#pragma once

#include "mhttp/tcpserver.hpp"
#include "mhttp/client.hpp"

#include "../gsl-lite.hpp"

#include <string_view>
#include <array>
#include <future>
#include <bitset>

#include "d8u/util.hpp"

namespace volstore
{
    using namespace std;
    using namespace gsl;

    using namespace mhttp;
    using namespace d8u;
    using namespace d8u::util;

    template <typename STORE, size_t U = 32, size_t M = 1024 * 1024> class BinaryStore
    {
        bool buffered_writes;
        TcpServer query;
        TcpServer read;
        TcpServer write;

        STORE& store;
    public:

        size_t ConnectionCount()
        {
            return query.ConnectionCount() + read.ConnectionCount() + write.ConnectionCount();
        }

        void Join()
        {
            query.Join();
            read.Join();
            write.Join();
        }

        void Shutdown()
        {
            query.Shutdown();
            read.Shutdown();
            write.Shutdown();
        }

        ~BinaryStore()
        {
            Shutdown();
        }

        BinaryStore(STORE& _store, string_view is_port = "9095", string_view read_port = "9096", string_view write_port = "9097", size_t threads = 1, bool _buffered_writes = false)
            : buffered_writes(_buffered_writes)
            , store(_store)
            , query((uint16_t)stoi(is_port.data()), ConnectionType::message,
                [&](auto* pc, auto req, auto body, void *reply)
                {
                    std::vector<uint8_t> buffer;
                    if (req.size() == 33)
                    {
                        buffer.resize(1);
                        buffer[0] = (char)store.ValidateStandard(gsl::span<uint8_t>(req.data()+1, (size_t)32));
                    }
                    else if (req.size() % U)
                    {
                        std::cout << "Query Dropping Connection" << std::endl;
                        pc->Close();

                        return;
                    }
                    else if (req.size() == U)
                    {
                        buffer.resize(1);
                        buffer[0] = (char)store.Is(req);
                    }
                    else
                    {
                        buffer.resize(8);
                        *( (uint64_t*)buffer.data() ) = store.Many<U>(req);
                    }

                    pc->ActivateWrite(reply, std::move( buffer ));

                }, true, TcpServer::Options { threads })
            , read((uint16_t)stoi(read_port.data()), ConnectionType::writemap32,
                [&](auto* pc, auto req, auto body, void* reply)
                {
                    if (req.size() != 32)
                    {
                        std::cout << "Read Dropping Connection" << std::endl;
                        pc->Close();

                        return;
                    }

                    pc->ActivateMap(reply,store.Map(req));

                }, true, { threads })
            , write((uint16_t)stoi(write_port.data()), (buffered_writes) ? ConnectionType::message : ConnectionType::readmap32,
                [&](auto* pc, auto header, auto body, void* reply)
                {
                    if (header.size() < 32)
                    {
                        std::cout << "write Dropping Connection" << std::endl;
                        pc->Close();

                        return;
                    }

                    uint32_t written = 0;
                    if (buffered_writes)
                    {
                        store.Write(gsl::span<uint8_t>(header.data(),(size_t)32), gsl::span<uint8_t>(header.data()+32, header.size()-32));
                        written = header.size() - 32;

                        std::vector<uint8_t> buffer(4);
                        *((uint32_t*)buffer.data()) = written;

                        pc->ActivateWrite(reply,std::move(buffer));
                    }
                    else
                    {
                        //Unbuffered mode moves data from the socket directly to disk:
                        //

                        auto [size, id] = Map32::DecodeHeader(header);

                        if (size > 1024 * 1024 * 8)
                            written = 0;
                        else
                        {
                            auto dest = store.Allocate(id, (size_t)size);

                            pc->Read(dest);
                            written = size;
                        }

                        std::vector<uint8_t> buffer(4);
                        *((uint32_t*)buffer.data()) = written;

                        pc->AsyncWrite(std::move(buffer));
                    }

                }, buffered_writes, { threads })
        {
        }
    };

    class BinaryStoreClient
    {
        EventClient query;
        EventClient read;
        EventClient write;

        tdb::MediumHashmapSafe db;
    public:
        BinaryStoreClient(std::string_view cache="127.0.0.1.cache",string_view _query = "127.0.0.1:9009", string_view _read = "127.0.0.1:1010", string_view _write = "127.0.0.1:1111")
            : db(cache)
            , query(_query,ConnectionType::message)
            , read(_read, ConnectionType::message)
            , write(_write, ConnectionType::message) { }

        template <typename T> std::vector<uint8_t> Read(const T& id)
        {
            return read.AsyncWriteWaitT(id).first;
        }

        template <typename T, typename Y> void Write(const T& id, const Y& payload)
        {
            write.AsyncWriteWait(join_memory(id,payload)); //Sub-optimal, todo write lists
        }

        template <typename T> bool Is(const T& id)
        {
            auto [ptr, exists] = db.InsertLock(*( (tdb::Key32*) id.data() ), uint64_t(0));

            if (exists)
                return true;

            auto [res,body] = query.AsyncWriteWaitT(id);

            return (res.size() == 1 ) ? res[0] > 0 : false;
        }

        template <size_t U, typename T> uint64_t Many(const T& ids)
        {
            auto limit = ids.size() / U;

            if (limit > 64)
                throw runtime_error("The max limit for Many is 64");

            std::bitset<64> cache_result;
            size_t cache_count = 0;

            for (size_t i = 0; i < limit; i++)
            {
                auto [ptr, exists] = db.InsertLock(*(((tdb::Key32*)ids.data()) + i), uint64_t(0));

                if (exists)
                {
                    cache_result[i] = 1;
                    cache_count++;
                }
            }

            if (cache_count == limit)
                return cache_result.to_ullong();

            std::vector<uint8_t> send(ids.size());
            std::copy(ids.begin(), ids.end(), send.begin());
            auto [res, body] = query.AsyncWriteWait(std::move(send));

            if(res.size()!=8)
                throw runtime_error("Bad reply");

            uint64_t result = *(uint64_t*)res.data();

            return result | cache_result.to_ullong();
        }

        template <typename T, typename V> bool Validate(const T& id, V v)
        {
            std::vector<uint8_t> cmd = { 1 };
            auto [res, body] = query.AsyncWriteWait(join_memory(cmd,id));

            return (res.size() == 1) ? res[0] > 0 : false;
        }
    };

    class BinaryStoreEventClient
    {
        EventClient query;
        EventClient read;
        EventClient write;

        tdb::MediumHashmapSafe db;
    public:
        BinaryStoreEventClient(std::string_view cache = "127.0.0.1.cache",string_view _query = "127.0.0.1:9009", string_view _read = "127.0.0.1:1010", string_view _write = "127.0.0.1:1111")
            : db(cache)
            , query(_query, ConnectionType::message)
            , read(_read, ConnectionType::message)
            , write(_write, ConnectionType::map32client) { }

        void Flush()
        {
            query.Flush();
            read.Flush();
            write.Flush();
        }

        template <typename T, typename F> void Read(const T& id, F f)
        {
            read.AsyncWriteCallbackT(id, [f = std::move(f)](auto result, auto body)
            {
                f(std::move(result));
            });
        }

        template <typename T, typename Y, typename F> void Write(const T& id, const Y& payload, F f)
        {
            return write.AsyncWriteCallback(join_memory(id, payload), [f = std::move(f)](auto result, auto body)
            {
                f();
            }); //Sub-optimal, todo write lists
        }

        template <typename T, typename F> void Is(const T& id, F f)
        {
            auto [ptr, exists] = db.InsertLock(id, uint64_t(0));

            if (exists)
            {
                f(true);
                return;
            }

            query.AsyncWriteCallbackT(id,[f = std::move(f)](auto result, auto body)
            {
                f((result.size() == 1) ? result[0] > 0 : false);
            });
        }

        template <size_t U, typename T, typename F> void Many(const T& ids, F f)
        {
            //!TODO USE CACHE!!!!
            std::cout << "IMPLEMENT CACHE TEST" << std::endl;

            auto limit = ids.size() / U;

            if (limit > 64)
                throw runtime_error("The max limit for Many is 64");

            query.AsyncWriteCallbackT(ids, [f = std::move(f)](auto result, auto body)
            {
                uint64_t res = *(uint64_t*)result.data();
                f(res);
            });
        }

        template <typename T, typename V> bool Validate(const T& id, V v) const
        {
            std::cout << "TODO NET-VALIDATE!!!" << std::endl;
            return true;//TODO request server to validate block without transport.
        }
    };
}