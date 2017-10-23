//
// Created by Sai Jiang on 17/10/22.
//
#include "common.h"

static const int32_t codec = kodoc_on_the_fly;

void TokenBucketInit(TokenBucket *tb, double rate)
{
    tb->ts = GetTS();
    tb->CurCapactiy = 0;
    tb->MaxCapacity = 4096;
    tb->LimitedRate = rate; // Unit: Byte/ms
}

void PutToken(TokenBucket *tb)
{
    if (tb->CurCapactiy >= tb->MaxCapacity) return;
    long Now = GetTS();
    assert(Now >= tb->ts);
    uint32_t reload = (uint32_t)((Now - tb->ts) * tb->LimitedRate);
    assert(reload >= 0);
    if (reload > 0) {
        tb->ts = Now;
        tb->CurCapactiy = min(tb->CurCapactiy + reload, tb->MaxCapacity);
    }
}

bool GetToken(TokenBucket *tb, size_t need)
{
    PutToken(tb);

    bool rval = false;

    if (tb->CurCapactiy >= need) {
        tb->CurCapactiy -= need;
        rval = true;
    }

    return rval;
}

Transmitter *Transmitter_Init(uint32_t maxsymbols, uint32_t maxsymbolsize)
{
    Transmitter *tx = malloc(sizeof(Transmitter));
    assert(tx != NULL);

    iqueue_init(&tx->src_queue);

    iqueue_init(&tx->sym_queue);

    iqueue_init(&tx->enc_queue);

    tx->enc_factory = kodoc_new_encoder_factory(
            codec, kodoc_binary8, maxsymbols, maxsymbolsize);

    tx->maxsymbol = maxsymbols;
    tx->maxsymbolsize = maxsymbolsize;
    tx->blksize = tx->maxsymbol * tx->maxsymbolsize;

    tx->NextBlockID = 0;

    tx->payload_size = kodoc_factory_max_payload_size(tx->enc_factory);
    tx->pktbuf = malloc(sizeof(Packet) + tx->payload_size);
    assert(tx->payload_size < 1500);

    struct sockaddr_in addr;

    tx->DataSock = socket(PF_INET, SOCK_DGRAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(PF_INET, DST_IP, &addr.sin_addr);
    addr.sin_port = htons(DST_DPORT);
    connect(tx->DataSock, (struct sockaddr *) &addr, sizeof(addr));

    tx->SignalSock = socket(PF_INET, SOCK_DGRAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SRC_SPORT);
    int rval = bind(tx->SignalSock, (struct sockaddr *)&addr, sizeof(struct sockaddr));
    assert(rval >= 0);
    int flags = fcntl(tx->SignalSock, F_GETFL, 0);
    fcntl(tx->SignalSock, F_SETFL, flags | O_NONBLOCK);

    return tx;
}

void Transmitter_Release(Transmitter *tx)
{
    assert(iqueue_is_empty(&tx->src_queue));
    assert(iqueue_is_empty(&tx->sym_queue));
    assert(iqueue_is_empty(&tx->enc_queue));

    kodoc_delete_factory(tx->enc_factory);

    free(tx->pktbuf);

    close(tx->DataSock);
    close(tx->SignalSock);

    free(tx);
}

// For Simplicity for now
size_t Send(Transmitter *tx, void *buf, size_t buflen)
{
    SrcData *inserted = malloc(sizeof(SrcData) + buflen);

    inserted->Len = sizeof(inserted->Len) + buflen;
    assert(inserted->Len == tx->maxsymbolsize);
    memcpy(inserted->rawdata, buf, buflen);

    iqueue_add_tail(&inserted->qnode, &tx->src_queue);

    return buflen;
}

// For Simplicity for now
void Div2Sym(Transmitter *tx)
{
    while (!iqueue_is_empty(&tx->src_queue)) {
        SrcData *sd = iqueue_entry(tx->src_queue.next, SrcData, qnode);
        iqueue_head *del = &sd->qnode;
        iqueue_del(del);
        Symbol *sym = malloc((sizeof(Symbol) + tx->maxsymbolsize));
        assert(sym != NULL);
        assert(sd->Len == tx->maxsymbolsize);
        memcpy(sym->data, sd->data, sd->Len);
        iqueue_add_tail(&sym->qnode, &tx->sym_queue);
        free(sd);
    }
}

void MovSym2Enc(Transmitter *tx)
{
    while (!iqueue_is_empty(&tx->sym_queue)) {
        EncWrapper *encwrapper = NULL;

        if (iqueue_is_empty(&tx->enc_queue) ||
                iqueue_entry(tx->enc_queue.prev, EncWrapper, qnode)->lrank == tx->maxsymbol) {
            encwrapper = malloc(sizeof(EncWrapper));
            encwrapper->enc = kodoc_factory_build_coder(tx->enc_factory);
            encwrapper->lrank = encwrapper->rrank = 0;
            encwrapper->id = tx->NextBlockID++;
            encwrapper->pblk = malloc(tx->blksize);
            TokenBucketInit(&encwrapper->tb, 200); // 5ms Gap
            iqueue_add_tail(&encwrapper->qnode, &tx->enc_queue);
        } else {
            encwrapper = iqueue_entry(tx->enc_queue.prev, EncWrapper, qnode);
        }

        assert(encwrapper != NULL);

        Symbol *sym = NULL;
        for (iqueue_head *p = tx->sym_queue.next, *nxt;
             p != &tx->sym_queue && encwrapper->lrank < tx->maxsymbolsize; p = nxt) {
            nxt = p->next;
            sym = iqueue_entry(p, Symbol, qnode);

            void *pdst = encwrapper->pblk + encwrapper->lrank * tx->maxsymbolsize;
            memcpy(pdst, sym->data, tx->maxsymbolsize);
            kodoc_set_const_symbol(encwrapper->enc, encwrapper->lrank, pdst, tx->maxsymbolsize);
            encwrapper->lrank = kodoc_rank(encwrapper->enc);

            tx->pktbuf->id = encwrapper->id;
            kodoc_write_payload(encwrapper->enc, tx->pktbuf->data);
            send(tx->DataSock, tx->pktbuf, sizeof(Packet) + tx->payload_size, 0);

            iqueue_del(&sym->qnode);
            free(sym);
        }
    }
}

void CheckACK(Transmitter *tx)
{
    AckMsg msg;

    while (true) {
        ssize_t nbytes = read(tx->SignalSock, &msg, sizeof(msg));
        if (nbytes < 0) break;
        assert(nbytes == sizeof(msg));

        EncWrapper *encwrapper = NULL;
        iqueue_foreach(encwrapper, &tx->enc_queue, EncWrapper, qnode) {
            if (msg.id > encwrapper->id) continue;
            else if (msg.id < encwrapper->id) break;
            else {
                assert(msg.id == encwrapper->id);
                assert(msg.rank > 0 && msg.rank <= tx->maxsymbol);
                encwrapper->rrank = max(encwrapper->rrank, msg.rank);
            }
        }
    }
}

void Fountain(Transmitter *tx)
{
    EncWrapper *encwrapper = NULL;
    for (iqueue_head *p = tx->enc_queue.next, *nxt; p != &tx->enc_queue; p = nxt) {
        nxt = p->next;
        encwrapper = iqueue_entry(p, EncWrapper, qnode);

        // free the encoder that finished the job
        if (encwrapper->lrank == tx->maxsymbol && encwrapper->rrank == tx->maxsymbol) {
            iqueue_del(&encwrapper->qnode);
            free(encwrapper->pblk);
            kodoc_delete_coder(encwrapper->enc);
            free(encwrapper);
            encwrapper = NULL;
        } else if (encwrapper->lrank > encwrapper->rrank && GetToken(&encwrapper->tb, sizeof(Packet))) {
            tx->pktbuf->id = encwrapper->id;
            kodoc_write_payload(encwrapper->enc, tx->pktbuf->data);
            send(tx->DataSock, tx->pktbuf, sizeof(Packet) + tx->payload_size, 0);
        }
    }
}


int main()
{
    Transmitter *tx = Transmitter_Init(MAXSYMBOL, MAXSYMBOLSIZE);

    uint32_t seq = 0;

    UserData_t ud;
    ud.ts = 0;

    do {
        long now = GetTS();

        if ((ud.ts == 0 || now - ud.ts >= 2) && seq < 2048)  {
            ud.seq = seq++;
            ud.ts = now;
            Send(tx, &ud, sizeof(ud));
        }

        Div2Sym(tx);
        MovSym2Enc(tx);
        CheckACK(tx);
        Fountain(tx);

        usleep(500);

    } while (seq < 2048 ||
            !iqueue_is_empty(&tx->src_queue) ||
            !iqueue_is_empty(&tx->enc_queue));

    Transmitter_Release(tx);
}