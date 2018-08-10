#include "homa_impl.h"
#define KSELFTEST_NOT_MAIN 1
#include "kselftest_harness.h"
#include "ccutils.h"
#include "mock.h"
#include "utils.h"

FIXTURE(homa_outgoing) {
	__be32 client_ip;
	int client_port;
	__be32 server_ip;
	int server_port;
	struct homa homa;
	struct homa_sock hsk;
	struct sockaddr_in server_addr;
};
FIXTURE_SETUP(homa_outgoing)
{
	self->client_ip = unit_get_in_addr("196.168.0.1");
	self->client_port = 40000;
	self->server_ip = unit_get_in_addr("1.2.3.4");
	self->server_port = 99;
	homa_init(&self->homa);
	mock_sock_init(&self->hsk, &self->homa, self->client_port,
			self->server_port);
	self->server_addr.sin_family = AF_INET;
	self->server_addr.sin_addr.s_addr = self->server_ip;
	self->server_addr.sin_port = htons(self->server_port);
	unit_log_clear();
}
FIXTURE_TEARDOWN(homa_outgoing)
{
	mock_sock_destroy(&self->hsk, &self->homa.port_map);
	homa_destroy(&self->homa);
	unit_teardown();
}

TEST_F(homa_outgoing, homa_message_out_init_basics)
{
	struct homa_client_rpc *crpc = homa_client_rpc_new(&self->hsk,
			&self->server_addr, 3000, NULL);
	EXPECT_FALSE(IS_ERR(crpc));
	EXPECT_EQ(1, unit_list_length(&self->hsk.client_rpcs));
	EXPECT_STREQ("csum_and_copy_from_iter_full copied 1400 bytes; "
		"csum_and_copy_from_iter_full copied 1400 bytes; "
		"csum_and_copy_from_iter_full copied 200 bytes", unit_log_get());
	unit_log_clear();
	unit_log_message_out_packets(&crpc->request, 1);
	EXPECT_STREQ("DATA from 0.0.0.0:40000, dport 99, id 1, length 1426, "
			"message_length 3000, offset 0, unscheduled 9800; "
		     "DATA from 0.0.0.0:40000, dport 99, id 1, length 1426, "
			"message_length 3000, offset 1400, unscheduled 9800; "
		     "DATA from 0.0.0.0:40000, dport 99, id 1, length 226, "
			"message_length 3000, offset 2800, unscheduled 9800",
		     unit_log_get());
}

TEST_F(homa_outgoing, homa_message_out_init__cant_alloc_skb)
{
	mock_alloc_skb_errors = 2;
	struct homa_client_rpc *crpc = homa_client_rpc_new(&self->hsk,
			&self->server_addr, 3000, NULL);
	EXPECT_TRUE(IS_ERR(crpc));
	EXPECT_EQ(ENOMEM, -PTR_ERR(crpc));
	EXPECT_EQ(0, unit_list_length(&self->hsk.client_rpcs));
}

TEST_F(homa_outgoing, homa_message_out_init__cant_copy_data)
{
	mock_copy_data_errors = 2;
	struct homa_client_rpc *crpc = homa_client_rpc_new(&self->hsk,
			&self->server_addr, 3000, NULL);
	EXPECT_TRUE(IS_ERR(crpc));
	EXPECT_EQ(EFAULT, -PTR_ERR(crpc));
	EXPECT_EQ(0, unit_list_length(&self->hsk.client_rpcs));
}

TEST_F(homa_outgoing, homa_xmit_to_sender__server_request)
{
	struct homa_server_rpc *srpc;
	
	srpc = unit_server_rpc(&self->hsk, SRPC_INCOMING, self->client_ip,
		self->server_ip, self->client_port, 1111, 10000, 10000);
	EXPECT_NE(NULL, srpc);
	
	struct sk_buff *skb = alloc_skb(HOMA_SKB_SIZE, GFP_KERNEL);
	skb_reserve(skb, HOMA_SKB_RESERVE);
	skb_reset_transport_header(skb);
	struct grant_header *h = (struct grant_header *) skb_put(skb,
			sizeof(*h));
	h->common.type = GRANT;
	h->offset = htonl(12345);
	h->priority = 4;
	mock_xmit_log_verbose = 1;
	homa_xmit_to_sender(skb, &srpc->request);
	EXPECT_STREQ("xmit GRANT from 0.0.0.0:99, dport 40000, id 1111, "
			"length 18, offset 12345, priority 4",
			unit_log_get());
}

TEST_F(homa_outgoing, homa_xmit_to_sender__client_response)
{
	struct homa_client_rpc *crpc;
	
	crpc = unit_client_rpc(&self->hsk, CRPC_INCOMING, self->client_ip,
		self->server_ip, self->server_port, 1111, 100, 10000);
	EXPECT_NE(NULL, crpc);
	unit_log_clear();
	
	struct sk_buff *skb = alloc_skb(HOMA_SKB_SIZE, GFP_KERNEL);
	skb_reserve(skb, HOMA_SKB_RESERVE);
	skb_reset_transport_header(skb);
	struct grant_header *h = (struct grant_header *) skb_put(skb,
			sizeof(*h));
	h->common.type = GRANT;
	h->offset = htonl(12345);
	h->priority = 4;
	mock_xmit_log_verbose = 1;
	homa_xmit_to_sender(skb, &crpc->response);
	EXPECT_STREQ("xmit GRANT from 0.0.0.0:40000, dport 99, id 1111, "
			"length 18, offset 12345, priority 4",
			unit_log_get());
}