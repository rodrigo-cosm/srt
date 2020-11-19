#include "gtest/gtest.h"
#include <thread>
#include <string>
#include "srt.h"
#include "netinet_any.h"

inline std::string operator""_S(const char* src, unsigned long )
{
    return std::string(src);
}

class TestIPv6
    : public ::testing::Test
{
protected:
    int yes = 1;
    int no = 0;

    TestIPv6()
    {
        // initialization code here
    }

    ~TestIPv6()
    {
        // cleanup any pending stuff, but no exceptions allowed
    }

protected:
    // SetUp() is run immediately before a test starts.
    void SetUp()
    {
        ASSERT_GE(srt_startup(), 0);

        m_caller_sock = srt_create_socket();
        ASSERT_NE(m_caller_sock, SRT_ERROR);

        m_listener_sock = srt_create_socket();
        ASSERT_NE(m_listener_sock, SRT_ERROR);
    }

    void TearDown()
    {
        // Code here will be called just after the test completes.
        // OK to throw exceptions from here if needed.
        srt_close(m_listener_sock);
        srt_close(m_caller_sock);
        srt_cleanup();
    }

public:
    void ClientThread(int family, const std::string& address)
    {
        sockaddr_any sa (family);
        sa.hport(m_listen_port);
        ASSERT_EQ(inet_pton(family, address.c_str(), sa.get_addr()), 1);

        std::cout << "Calling: " << address << "(" << fam[family] << ")\n";

        ASSERT_NE(srt_connect(m_caller_sock, sa.get(), sa.size()), SRT_ERROR);

        int32_t ipv6_zero [] = {0, 0, 0, 0};

        ASSERT_NE(srt_getsockname(m_caller_sock, sa.get(), &sa.len), SRT_ERROR);
        ShowAddress("CALLER srt_getsockname", sa);
        EXPECT_NE(memcmp(ipv6_zero, sa.get_addr(), sizeof ipv6_zero), 0)
            << "EMPTY address in srt_getsockname";

        ASSERT_NE(srt_getpeername(m_caller_sock, sa.get(), &sa.len), SRT_ERROR);
        ShowAddress("CALLER srt_getpeername", sa);
        EXPECT_NE(memcmp(ipv6_zero, sa.get_addr(), sizeof ipv6_zero), 0)
            << "EMPTY address in srt_getpeername";
    }

    std::map<int, std::string> fam = { {AF_INET, "IPv4"}, {AF_INET6, "IPv6"} };

    void ShowAddress(std::string src, const sockaddr_any& w)
    {
        ASSERT_NE(fam.count(w.family()), 0) << "INVALID FAMILY";

        char out[256];
        ASSERT_NE(inet_ntop(w.family(), w.get_addr(), out, 256), nullptr);
        std::cout << src << ": " << out << "(" << fam[w.family()] << ")" << std::endl;
    }

    void DoAccept()
    {
        sockaddr_any sc1;

        SRTSOCKET accepted_sock = srt_accept(m_listener_sock, sc1.get(), &sc1.len);
        ASSERT_NE(accepted_sock, SRT_INVALID_SOCK);

        ShowAddress("Accepted", sc1);

        sockaddr_any sn;
        EXPECT_NE(srt_getsockname(accepted_sock, sn.get(), &sn.len), SRT_ERROR);

        ShowAddress("Accepted srt_getsockname", sn);

        int32_t ipv6_zero [] = {0, 0, 0, 0};
        EXPECT_NE(memcmp(ipv6_zero, sn.get_addr(), sizeof ipv6_zero), 0)
            << "EMPTY address in srt_getsockname";

        EXPECT_NE(srt_getpeername(accepted_sock, sn.get(), &sn.len), SRT_ERROR);

        ShowAddress("Accepted srt_getpeername", sn);
        EXPECT_NE(memcmp(ipv6_zero, sn.get_addr(), sizeof ipv6_zero), 0)
            << "EMPTY address in srt_getpeername";

        srt_close(accepted_sock);
    }

protected:
    SRTSOCKET m_caller_sock;
    SRTSOCKET m_listener_sock;
    const int m_listen_port = 4200;
};


TEST_F(TestIPv6, v4_calls_v6_mapped)
{
    sockaddr_any sa (AF_INET6);
    sa.hport(m_listen_port);

    ASSERT_EQ(srt_setsockflag(m_listener_sock, SRTO_IPV6ONLY, &no, sizeof no), 0);
    ASSERT_NE(srt_bind(m_listener_sock, sa.get(), sa.size()), SRT_ERROR);
    ASSERT_NE(srt_listen(m_listener_sock, SOMAXCONN), SRT_ERROR);

    std::thread client(&TestIPv6::ClientThread, this, AF_INET, "127.0.0.1"_S);

    DoAccept();

    client.join();
}

TEST_F(TestIPv6, v6_calls_v6_mapped)
{
    sockaddr_any sa (AF_INET6);
    sa.hport(m_listen_port);

    ASSERT_EQ(srt_setsockflag(m_listener_sock, SRTO_IPV6ONLY, &no, sizeof no), 0);
    ASSERT_NE(srt_bind(m_listener_sock, sa.get(), sa.size()), SRT_ERROR);
    ASSERT_NE(srt_listen(m_listener_sock, SOMAXCONN), SRT_ERROR);

    std::thread client(&TestIPv6::ClientThread, this, AF_INET6, "::1"_S);

    DoAccept();

    client.join();
}


TEST_F(TestIPv6, v6_calls_v6)
{
    sockaddr_any sa (AF_INET6);
    sa.hport(m_listen_port);

    // This time bind the socket exclusively to IPv6.
    ASSERT_EQ(srt_setsockflag(m_listener_sock, SRTO_IPV6ONLY, &yes, sizeof yes), 0);
    ASSERT_EQ(inet_pton(AF_INET6, "::1", sa.get_addr()), 1);

    ASSERT_NE(srt_bind(m_listener_sock, sa.get(), sa.size()), SRT_ERROR);
    ASSERT_NE(srt_listen(m_listener_sock, SOMAXCONN), SRT_ERROR);

    std::thread client(&TestIPv6::ClientThread, this, AF_INET6, "::1"_S);

    DoAccept();

    client.join();
}

TEST_F(TestIPv6, v6_calls_v4)
{
    sockaddr_any sa (AF_INET);
    sa.hport(m_listen_port);

    // This time bind the socket exclusively to IPv4.
    ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", sa.get_addr()), 1);

    ASSERT_NE(srt_bind(m_listener_sock, sa.get(), sa.size()), SRT_ERROR);
    ASSERT_NE(srt_listen(m_listener_sock, SOMAXCONN), SRT_ERROR);

    std::thread client(&TestIPv6::ClientThread, this, AF_INET6, "0::FFFF:127.0.0.1"_S);

    DoAccept();

    client.join();
}

