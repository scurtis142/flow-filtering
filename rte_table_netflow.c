/*-
 *   BSD LICENSE
 * 
 *   Copyright(c) 2014, Choonho Son choonho.som@gmail.com
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <rte_common.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_byteorder.h>
#include <rte_hash_crc.h>

#include "rte_table_netflow.h"

uint64_t global_packet_count;

void *
rte_table_netflow_create(void *params, int socket_id, uint32_t entry_size)
{
    struct rte_table_netflow_params *p = 
        (struct rte_table_netflow_params *) params;

    struct rte_table_netflow *t;
    uint32_t total_cl_size, total_size;
    uint32_t i;

    if (p->n_entries > MAX_ENTRY) {
        RTE_LOG(ERR, TABLE, "Entry is large than MAX_ENTRY(%d)\n", (uint32_t)MAX_ENTRY);
        p->n_entries = MAX_ENTRY;
    }

    /* Check input parameters */
    if ((p == NULL) ||
        (p->n_entries == 0) ||
        (!rte_is_power_of_2(p->n_entries)) ) {
        return NULL;
    }

    /* Memory allocation */
    total_cl_size = (sizeof(struct rte_table_netflow) +
            RTE_CACHE_LINE_SIZE) / RTE_CACHE_LINE_SIZE;
    total_cl_size += (p->n_entries * sizeof(hashBucket_t*) + 
            RTE_CACHE_LINE_SIZE) / RTE_CACHE_LINE_SIZE;
    total_size = total_cl_size * RTE_CACHE_LINE_SIZE;
    t = rte_zmalloc_socket("TABLE", total_size, RTE_CACHE_LINE_SIZE, socket_id);
    if (t == NULL) {
        RTE_LOG(ERR, TABLE,
            "%s: Cannot allocate %u bytes for netflow table\n",
            __func__, total_size);
        return NULL;
    }

    /* Spinlock initialization */
    for (i = 0; i < p->n_entries; i++) {
        rte_spinlock_init(&t->lock[i]);
    }

    /* Memory initialzation */
    t->entry_size = entry_size;
    t->n_entries = p->n_entries;
    t->f_hash = p->f_hash;
    t->seed = p->seed;

    global_packet_count = 0;

    return t;
}

int
rte_table_netflow_entry_add(
    void *table,
    void *key,
    void *entry)
{
    struct rte_table_netflow *t = (struct rte_table_netflow *)table;
    union rte_table_netflow_key *k = key;
    struct ipv4_hdr *ip = entry;
    struct tcp_hdr *tcp;
    hashBucket_t *previous_pointer = NULL;
    hashBucket_t *bucket = NULL;
    hashBucket_t *bkt = NULL;
    uint32_t idx = 0;
    uint8_t updated = 0; 
    uint8_t notfound = 0; 
    struct timeval curr;

#if DEBUG
  	printf ("src_ip = %d\n", k->ip_src);
	printf ("dst_ip = %d\n", k->ip_dst);
	printf ("proto = %d\n", k->proto);
	printf ("src_port = %d\n", k->port_src);
	printf ("dst_port = %d\n", k->port_dst);
#endif
    /* hashing with SSE4_2 CRC32 */ 
    idx = rte_hash_crc_4byte(k->proto, idx);
    idx = rte_hash_crc_4byte(k->ip_src, idx);
    idx = rte_hash_crc_4byte(k->ip_dst, idx);
    idx = rte_hash_crc_4byte(k->port_src, idx);
    idx = rte_hash_crc_4byte(k->port_dst, idx);
    idx = idx % t->n_entries;
    
    /****************************************************************
     * Lock one entry (t->array[idx]'s lock = t->lock[idex]
     *
     * So netflow_export can use other entries 
     ****************************************************************/
    rte_spinlock_lock(&t->lock[idx]);
 
    bucket = t->array[idx];
    previous_pointer = bucket;
    
    while (bucket != NULL) {
        /* Find same flow in the bucket's list */
        if ((bucket->ip_src == k->ip_src) && (bucket->ip_dst == k->ip_dst) ) {
            /* accumulated ToS Field */
            bucket->src2dstTos |= ip->type_of_service;

            /* accumulated TCP Flags */
            if (k->proto == IPPROTO_TCP) {
                tcp = (struct tcp_hdr *)((unsigned char*)ip + sizeof(struct ipv4_hdr));
                bucket->src2dstTcpFlags |= tcp->tcp_flags;
            }

            /* accumulated Bytes */
            /* TODO: if bytesSent > 2^32, netflow v5 value is wrong
             *  since, netflow v5 dOctet is 32bit.
             */
            bucket->bytesSent += rte_cpu_to_be_16(ip->total_length);
            bucket->pktSent++;

            /* Time */
            gettimeofday(&curr, NULL);
            bucket->lastSeenSent = curr;

            updated = 1;
            break;
        }
        printf("Bucket collision\n");
        notfound = 1;
        previous_pointer = bucket;
        bucket = bucket->next;
    }

    if( !updated ) {
        /* Create New Bucket */
        //printf("First Seen : %" PRIu32 "\n", idx);
        bkt = (hashBucket_t *)rte_zmalloc("BUCKET", sizeof(hashBucket_t), RTE_CACHE_LINE_SIZE);
        bkt->magic = 1;
        bkt->vlanId     = k->vlanId;
        bkt->proto      = k->proto;
        bkt->ip_src     = k->ip_src;
        bkt->ip_dst     = k->ip_dst;
        bkt->port_src   = k->port_src;
        bkt->port_dst   = k->port_dst;
    
        /* ToS Field */
        bkt->src2dstTos = ip->type_of_service; 
        
        /* TCP Flags */
        if (k->proto == IPPROTO_TCP) {
            tcp = (struct tcp_hdr *)((unsigned char*)ip + sizeof(struct ipv4_hdr));
            bkt->src2dstTcpFlags = tcp->tcp_flags;

            /* TODO: If TCP flags is start of Flow (Syn) 
             * Save payload of DPI 
             */

            /* If Flags is FIN, check and of flow */
        }

        /* Bytes (Total number of Layer 3 bytes)  */
        bkt->bytesSent = rte_cpu_to_be_16(ip->total_length);
        bkt->pktSent++;

        /* Time */
        gettimeofday(&curr, NULL);
        bkt->firstSeenSent = bkt->lastSeenSent = curr; 
        
        /* Update contents of bucket */
        if (notfound) previous_pointer->next = bkt;
        else t->array[idx] = bkt;
    }
    
    rte_spinlock_unlock(&t->lock[idx]);
    /***********************************************************************
     * End of entry lock
     * release lock
     **********************************************************************/
    global_packet_count++;
    return 1;
}

int
rte_table_netflow_free(void *table)
{
    struct rte_table_netflow *t = (struct rte_table_netflow *)table;
    
    /* Check input paramters */
    if (t == NULL) {
        RTE_LOG(ERR, TABLE, "%s: table parameter is NULL\n", __func__);
        return -EINVAL;
    }

    /* Free previously allocated resources */
    rte_free(t);
    return 0;
}

#if 0
struct rte_table_ops rte_table_netflow_ops = {
    .f_create = rte_table_netflow_create,
    .f_free   = rte_table_netflow_free,
    .f_add    = rte_table_netflow_entry_add,
    .f_delete = NULL,
    .f_lookup = NULL, /* rte_table_netflow_lookup, */
};
#endif

int
rte_table_print(void *table)
{
	struct rte_table_netflow *t = (struct rte_table_netflow *)table;
	hashBucket_t *bkt;

	printf ("\nprinting flow table\n");
	printf ("t->n_entries = %d\n", t->n_entries);
	
	for (unsigned int i = 0; i < t->n_entries; i++) {
		rte_spinlock_lock(&t->lock[i]);
		bkt = t->array[i];
		if (bkt != NULL) {
			printf ("src_ip = %d\ndst_ip = %d\nsrc_port = %d\ndst_port = %d\nproto = %d\n",
					bkt->ip_src, bkt->ip_dst, bkt->port_src, bkt->port_dst, bkt->proto);
			printf ("bytes_sent = %ld\nbytes_recv = %ld\npackets_sent = %ld\npackets_recv = %ld\n\n",
					bkt->bytesSent, bkt->bytesRcvd, bkt->pktSent, bkt->pktRcvd);
		}
        rte_spinlock_unlock(&t->lock[i]);
	}

	return 0;
}


void
rte_table_print_packet_count (void) {
   fprintf (stderr, "Total Packets Decoded: %lu\n", global_packet_count);
}


int
rte_table_print_stats(void *table)
{
	struct rte_table_netflow *t = (struct rte_table_netflow *)table;
	hashBucket_t *bkt;

   uint64_t total_bytes = 0;
   uint64_t total_pkts  = 0;
   uint64_t total_flows = 0;

   printf ("\nprinting flow table statistics\n");
   printf ("t->n_entries = %d\n", t->n_entries);

   for (unsigned int i = 0; i < t->n_entries; i++) {
      rte_spinlock_lock(&t->lock[i]);
		bkt = t->array[i];
		while (bkt != NULL) {
         total_bytes += bkt->bytesSent;
         total_pkts  += bkt->pktSent;
         total_flows++;
         bkt = bkt->next;
		}
      rte_spinlock_unlock(&t->lock[i]);
	}

   printf ("total flows = %lu\n", total_flows);
   printf ("total bytes = %lu\n", total_bytes);
   printf ("total pkts  = %lu\n", total_pkts);


	return 0;
}


void rte_table_export_to_file (const char *filename) {

   int buf_size = EXPORT_BUF_INITAL_SIZE;
   int fd;
   int snp_res;
   size_t buf_end_offset = 0;
   char *buf;
   const char *tmpfile = "/tmp/netflow-export-tmp.csv";
   hashBucket_t *bucket;
   struct in_addr src_addr;
   struct in_addr dst_addr;
   char src_ip_str[16];
   char dst_ip_str[16];

   if ((buf = malloc (sizeof (char) * buf_size)) == NULL) {
      printf ("malloc failed with %s\n", strerror (errno));
      exit (1);
   }

   for (unsigned int i = 0; i < table->n_entries; i++) {
		rte_spinlock_lock(&table->lock[i]);
		bucket = table->array[i];
		while (bucket != NULL) {
         /* Free space needed in buffer is maximum number of digits needed to represent
          * an entry which is 91(including null byte) */
         if ((buf_size - buf_end_offset) <= 91) {
            if ((buf = realloc (buf, buf_size * 2)) == NULL) {
               printf ("realloc failed with error %s\n", strerror (errno));
               exit (1);
            } else {
               buf_size *= 2;
            }
         }
         /* Need to copy the string here rather than use directly in the sprintf
            because inet_ntoa return a static buffer that get written over on
            subsequent calls */
         src_addr.s_addr = bucket->ip_src;
         dst_addr.s_addr = bucket->ip_dst;
         strcpy(src_ip_str, inet_ntoa(src_addr));
         strcpy(dst_ip_str, inet_ntoa(dst_addr));

         snp_res = snprintf ((buf + buf_end_offset), 91, "%s,%s,%d,%d,%d,%lu,%lu\n",
               src_ip_str,
               dst_ip_str,
               bucket->port_src,
               bucket->port_dst,
               bucket->proto,
               bucket->bytesSent,
               bucket->pktSent);
         if (snp_res < 0) {
            printf ("sprintf failed with %s\n", strerror (errno));
            exit (1);
         }
         buf_end_offset += snp_res;
         bucket = bucket->next;
		}
      rte_spinlock_unlock(&table->lock[i]);
	}

   /* More effeciant to just do a single write */
   if ((fd = open (tmpfile, O_WRONLY | O_CREAT, S_IRWXU | S_IRWXO)) < 0) { /* Returns non-negative integer on success */
      printf ("open failed with error %s\n", strerror (errno));
      exit (1);
   }

   if((int)buf_end_offset != write (fd, buf, buf_end_offset)) {
      printf ("write didn't return expected number of bytes\n");
      exit (1);
   }

   if (close (fd) != 0) { /* Returns 0 on success */
      printf ("close failed with error %s\n", strerror (errno));
      exit (1);
   }

   rename (tmpfile, filename);
   return;
}
