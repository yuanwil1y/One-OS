#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "nmap_l2.h"
#include "nmap_internal.h"

static void test_timing(void)
{
    nmap_timing_policy_t p = nmap_timing_policy_default();
    assert(nmap_internal_timing_validate(&p) == ESP_OK);
    p.max_inflight = 0;
    assert(nmap_internal_timing_validate(&p) == ESP_ERR_INVALID_ARG);
    p = nmap_timing_policy_default();
    p.max_inflight = NMAP_MAX_CONCURRENCY + 1;
    assert(nmap_internal_timing_validate(&p) == ESP_ERR_INVALID_ARG);
    p = nmap_timing_policy_default();
    p.max_retries = NMAP_MAX_RETRIES + 1;
    assert(nmap_internal_timing_validate(&p) == ESP_ERR_INVALID_ARG);
    p = nmap_timing_policy_default();
    p.max_probes_per_second = 51;
    assert(nmap_internal_timing_validate(&p) == ESP_ERR_INVALID_ARG);
    p = nmap_timing_policy_default();
    p.probe_timeout_ms = 49;
    assert(nmap_internal_timing_validate(&p) == ESP_ERR_INVALID_ARG);
    p = nmap_timing_policy_default();
    p.scan_timeout_ms = 300;
    p.probe_timeout_ms = 400;
    assert(nmap_internal_timing_validate(&p) == ESP_ERR_INVALID_ARG);
}

static void test_connect_mapping(void)
{
    nmap_port_state_t state;
    nmap_reason_t reason;
    nmap_internal_classify_connect_error(0, &state, &reason);
    assert(state == NMAP_PORT_OPEN && reason == NMAP_REASON_CONNECTION_ACCEPTED);
    nmap_internal_classify_connect_error(ECONNREFUSED, &state, &reason);
    assert(state == NMAP_PORT_CLOSED && reason == NMAP_REASON_CONNECTION_REFUSED);
    nmap_internal_classify_connect_error(ETIMEDOUT, &state, &reason);
    assert(state == NMAP_PORT_FILTERED && reason == NMAP_REASON_TIMEOUT);
    nmap_internal_classify_connect_error(EHOSTUNREACH, &state, &reason);
    assert(state == NMAP_PORT_UNREACHABLE && reason == NMAP_REASON_HOST_UNREACHABLE);
    nmap_internal_classify_connect_error(ENETUNREACH, &state, &reason);
    assert(state == NMAP_PORT_UNREACHABLE && reason == NMAP_REASON_NETWORK_UNREACHABLE);
    nmap_internal_classify_connect_error(ENOMEM, &state, &reason);
    assert(state == NMAP_PORT_ERROR && reason == NMAP_REASON_LOCAL_RESOURCE);
    nmap_internal_classify_connect_error(EINVAL, &state, &reason);
    assert(state == NMAP_PORT_ERROR && reason == NMAP_REASON_IO_ERROR);
}

static void test_ipv4_helpers(void)
{
    assert(nmap_internal_ipv4_is_unicast(inet_addr("192.168.1.2")));
    assert(nmap_internal_ipv4_is_unicast(inet_addr("169.254.1.2")));
    assert(!nmap_internal_ipv4_is_unicast(inet_addr("0.0.0.0")));
    assert(!nmap_internal_ipv4_is_unicast(inet_addr("255.255.255.255")));
    assert(!nmap_internal_ipv4_is_unicast(inet_addr("224.0.0.1")));
    assert(nmap_internal_netmask_prefix(inet_addr("255.255.255.0")) == 24);
    assert(nmap_internal_netmask_prefix(inet_addr("255.255.255.252")) == 30);
    assert(nmap_internal_netmask_prefix(inet_addr("255.255.0.255")) == -1);
}

static void assert_service(const char *wire, bool active_http, nmap_service_kind_t kind, const char *name)
{
    nmap_service_result_t out;
    nmap_internal_service_match((const uint8_t *)wire, strlen(wire), false, active_http, &out);
    assert(out.service == kind);
    assert(strcmp(out.service_name, name) == 0);
    assert(out.confidence == NMAP_SERVICE_CONFIDENCE_STRONG);
}

static void test_service_matching(void)
{
    nmap_service_result_t out;

    assert_service("SSH-2.0-OpenSSH_9.7\r\n", false, NMAP_SERVICE_SSH, "ssh");
    nmap_internal_service_match((const uint8_t *)"SSH-2.0-OpenSSH_9.7\r\n",
                                strlen("SSH-2.0-OpenSSH_9.7\r\n"), false, false, &out);
    assert(strcmp(out.product, "OpenSSH_9.7") == 0);
    assert(strcmp(out.probe_id, "passive-ssh-rfc4253") == 0);

    assert_service("HTTP/1.1 200 OK\r\nServer: nginx/1.26.2\r\nContent-Length: 0\r\n\r\n",
                   true, NMAP_SERVICE_HTTP, "http");
    nmap_internal_service_match((const uint8_t *)"HTTP/1.1 200 OK\r\nServer: nginx/1.26.2\r\n\r\n",
                                strlen("HTTP/1.1 200 OK\r\nServer: nginx/1.26.2\r\n\r\n"), false, true, &out);
    assert(strcmp(out.product, "nginx/1.26.2") == 0);
    assert((out.evidence_flags & NMAP_SERVICE_EVIDENCE_ACTIVE_PROBE) != 0);

    assert_service("220 ftp.example FTP server ready\r\n", false, NMAP_SERVICE_FTP, "ftp");
    assert_service("220 mail.example ESMTP Postfix\r\n", false, NMAP_SERVICE_SMTP, "smtp");

    nmap_internal_service_match((const uint8_t *)"220 generic service ready\r\n",
                                strlen("220 generic service ready\r\n"), false, false, &out);
    assert(out.service == NMAP_SERVICE_UNKNOWN);
    assert(out.confidence == NMAP_SERVICE_CONFIDENCE_NONE);

    nmap_internal_service_match((const uint8_t *)"prefix SSH-2.0-OpenSSH\r\n",
                                strlen("prefix SSH-2.0-OpenSSH\r\n"), false, false, &out);
    assert(out.service == NMAP_SERVICE_UNKNOWN);

    nmap_internal_service_match((const uint8_t *)"hello HTTP/1.1 200 OK",
                                strlen("hello HTTP/1.1 200 OK"), false, true, &out);
    assert(out.service == NMAP_SERVICE_UNKNOWN);

    const uint8_t binary[] = {0x00, 0xff, 0x01, 0x02, 'F', 'T', 'P'};
    nmap_internal_service_match(binary, sizeof(binary), true, false, &out);
    assert(out.service == NMAP_SERVICE_UNKNOWN);
    assert((out.result_flags & NMAP_RESULT_TRUNCATED) != 0);
    assert(out.banner[0] == '.');

    nmap_internal_service_match(NULL, 0, false, false, &out);
    assert((out.evidence_flags & NMAP_SERVICE_EVIDENCE_PASSIVE_BANNER) == 0);
}

static void test_probe_plan(void)
{
    assert(nmap_internal_service_profile_for_port(NMAP_SERVICE_PROBE_AUTO, 80) == NMAP_SERVICE_PROBE_HTTP_HEAD);
    assert(nmap_internal_service_profile_for_port(NMAP_SERVICE_PROBE_AUTO, 8080) == NMAP_SERVICE_PROBE_HTTP_HEAD);
    assert(nmap_internal_service_profile_for_port(NMAP_SERVICE_PROBE_AUTO, 22) == NMAP_SERVICE_PROBE_PASSIVE);
    assert(nmap_internal_service_profile_for_port(NMAP_SERVICE_PROBE_HTTP_HEAD, 1234) == NMAP_SERVICE_PROBE_HTTP_HEAD);
    assert(nmap_internal_service_profile_for_port(NMAP_SERVICE_PROBE_PASSIVE, 80) == NMAP_SERVICE_PROBE_PASSIVE);
}

int main(void)
{
    test_timing();
    test_connect_mapping();
    test_ipv4_helpers();
    test_service_matching();
    test_probe_plan();
    puts("nmap_l2 host tests: PASS");
    return 0;
}
