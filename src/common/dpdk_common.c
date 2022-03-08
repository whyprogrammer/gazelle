/*
* Copyright (c) Huawei Technologies Co., Ltd. 2020-2021. All rights reserved.
* gazelle is licensed under the Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*     http://license.coscl.org.cn/MulanPSL2
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
* PURPOSE.
* See the Mulan PSL v2 for more details.
*/

#include <rte_kni.h>
#include <rte_bus_pci.h>
#include <rte_ethdev.h>
#include <rte_bus_pci.h>
#include <rte_mbuf.h>
#include <securec.h>

#include "dpdk_common.h"

#define GAZELLE_KNI_IFACES_NUM               1
#define GAZELLE_KNI_READ_SIZE                32
#define GAZELLE_MAX_PKT_SZ                   2048

#ifdef LTRAN_COMPILE
#include "ltran_log.h"
#define  COMMON_ERR(fmt, ...)    LTRAN_ERR(fmt, ##__VA_ARGS__)
#define  COMMON_INFO(fmt, ...)   LTRAN_INFO(fmt, ##__VA_ARGS__)
#else
#include "lstack_log.h"
#define  COMMON_ERR(fmt, ...)    LSTACK_LOG(ERR, LSTACK, fmt, ##__VA_ARGS__)
#define  COMMON_INFO(fmt, ...)   LSTACK_LOG(INFO, LSTACK, fmt, ##__VA_ARGS__)
#endif

static pthread_mutex_t g_kni_mutex = PTHREAD_MUTEX_INITIALIZER;
struct rte_kni *g_pkni = NULL;

/*
 * lock for preventing data race between tx thread and down operation.
 * Don't need to add lock on rx because down operation and rx are in the same thread
 */
pthread_mutex_t *get_kni_mutex(void)
{
    return &g_kni_mutex;
}

struct rte_kni* get_gazelle_kni(void)
{
    return g_pkni;
}

static int32_t kni_config_network_interface(uint16_t port_id, uint8_t if_up)
{
    int32_t ret = 0;
    static bool g_bond_dev_started = false;

    if (port_id >= rte_eth_dev_count_avail() || port_id >= RTE_MAX_ETHPORTS) {
        COMMON_ERR("Invalid port id %d \n", port_id);
        return -EINVAL;
    }

    if (if_up != 0) { /* Configure network interface up */
        if (!g_bond_dev_started) {
            pthread_mutex_lock(&g_kni_mutex);
            ret = rte_eth_dev_start(port_id);
            pthread_mutex_unlock(&g_kni_mutex);
            if (ret < 0) {
                COMMON_ERR("Failed to start port %d ret=%d\n", port_id, ret);
            }
            g_bond_dev_started = true;
        } else {
            COMMON_INFO("trying to start a started dev. \n");
        }
    } else {  /* Configure network interface down */
        if (g_bond_dev_started) {
            pthread_mutex_lock(&g_kni_mutex);
            rte_eth_dev_stop(port_id);
            pthread_mutex_unlock(&g_kni_mutex);
            g_bond_dev_started = false;
        } else {
            COMMON_INFO("trying to stop a stopped dev. \n");
        }
    }

    COMMON_INFO("Configure network interface of %d %s \n", port_id, if_up ? "up" : "down");
    return ret;
}

int32_t dpdk_kni_init(uint16_t port, struct rte_mempool *pool)
{
    int32_t ret;
    struct rte_kni_ops ops;
    struct rte_kni_conf conf;
    const struct rte_bus *bus = NULL;
    struct rte_eth_dev_info dev_info;
    const struct rte_pci_device *pci_dev = NULL;

    if (port >= RTE_MAX_ETHPORTS) {
        COMMON_ERR("Bond port id out of range.\n");
        return -1;
    }

    ret = rte_kni_init(GAZELLE_KNI_IFACES_NUM);
    if (ret < 0) {
        COMMON_ERR("rte_kni_init failed, errno: %d.\n", ret);
        return -1;
    }

    (void)memset_s(&dev_info, sizeof(dev_info), 0, sizeof(dev_info));
    (void)memset_s(&conf, sizeof(conf), 0, sizeof(conf));
    (void)memset_s(&ops, sizeof(ops), 0, sizeof(ops));

    ret = snprintf_s(conf.name, RTE_KNI_NAMESIZE, RTE_KNI_NAMESIZE - 1, "%s", GAZELLE_KNI_NAME);
    if (ret < 0) {
        COMMON_ERR("snprintf_s failed. ret=%d\n", ret);
        return -1;
    }
    conf.mbuf_size = GAZELLE_MAX_PKT_SZ;
    conf.group_id = port;

    rte_eth_dev_info_get(port, &dev_info);
    if (dev_info.device) {
        bus = rte_bus_find_by_device(dev_info.device);
    }
    if (bus && !strcmp(bus->name, "pci")) {
        pci_dev = RTE_DEV_TO_PCI(dev_info.device);
        conf.id = pci_dev->id;
        conf.addr = pci_dev->addr;
    }

    ops.change_mtu = NULL;
    ops.config_network_if = kni_config_network_interface;
    ops.port_id = port;
    g_pkni = rte_kni_alloc(pool, &conf, &ops);
    if (g_pkni == NULL) {
        COMMON_ERR("Fail to create kni for port: %d \n", port);
        return -1;
    }
    return 0;
}

int32_t kni_process_tx(struct rte_mbuf **pkts_burst, uint32_t count)
{
    uint32_t i = rte_kni_tx_burst(g_pkni, pkts_burst, count);
    for (; i < count; ++i) {
        rte_pktmbuf_free(pkts_burst[i]);
        pkts_burst[i] = NULL;
    }

    return 0;
}

void kni_process_rx(uint16_t port)
{
    uint16_t nb_kni_rx, nb_rx, i;
    struct rte_mbuf *pkts_burst[GAZELLE_KNI_READ_SIZE];

    nb_kni_rx = rte_kni_rx_burst(g_pkni, pkts_burst, GAZELLE_KNI_READ_SIZE);
    if (nb_kni_rx > 0) {
        pthread_mutex_lock(&g_kni_mutex);
        nb_rx = rte_eth_tx_burst(port, 0, pkts_burst, nb_kni_rx);
        pthread_mutex_unlock(&g_kni_mutex);

        for (i = nb_rx; i < nb_kni_rx; ++i) {
            rte_pktmbuf_free(pkts_burst[i]);
            pkts_burst[i] = NULL;
        }
    }
}