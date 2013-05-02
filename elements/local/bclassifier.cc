#include <click/config.h>
#include <click/error.hh>
#include <click/glue.hh>
#include <click/hvputils.hh>
#include <click/confparse.hh>
#include <click/args.hh>
#include <arpa/inet.h>
#include <sys/time.h>
#include <click/atomic.hh>
#include "bclassifier.hh"

CLICK_DECLS

BClassifier::BClassifier() : _test(0),
			     _batcher(0)
{
    _anno_offset = -1;
    _slice_offset = -1;
    _on_cpu = 0;
}

BClassifier::~BClassifier()
{
}

int
BClassifier::configure(Vector<String> &conf, ErrorHandler *errh)
{
    _div = false;
    if (cp_va_kparse(conf, this, errh,
		     "BATCHER", cpkN, cpElementCast, "Batcher", &_batcher,
		     "TEST", cpkN, cpInteger, &_test,
		     "DIV", cpkN, cpBool, &_div,		     
		     "CPU", cpkN, cpInteger, &_on_cpu, // 0: GPU, 1: CPU+batch, 2: CPU no batch
		     cpEnd) < 0)
	return -1;

    if (_on_cpu == 3) {
	errh->message("Skip mode.");
	return 0;
    }
    
    if (_on_cpu < 2)
	if (_batcher->req_anno(0, 4, BatchProducer::anno_write)) {
	    errh->error("Register annotation request in batcher failed");
	    return -1;
	}

    _psr.start = EthernetBatchProducer::ip4_hdr; // 14
    _psr.start_offset = 8; // TTL
    _psr.len = 16;
    _psr.end = _psr.start+_psr.start_offset+_psr.len;

    if (_on_cpu < 2)
	if (_batcher->req_slice_range(_psr) < 0) {
	    errh->error("Request slice range failed: %d, %d, %d, %d",
			_psr.start, _psr.start_offset, _psr.len, _psr.end);
	    return -1;
	}

    if (_test < 2) {
	errh->error("For now, we need random generated patterns, use a TEST"
		    " larger than 2 to assign #ptns");
	return -1;
    }

    return 0;
}

void
BClassifier::generate_random_patterns(g4c_pattern_t *ptns, int n)
{
    struct timeval tv;
    gettimeofday(&tv, 0);

    srandom((unsigned)(tv.tv_usec));

    for (int i=0; i<n; i++) {
	int nbits = random()%5;
	if (random()%3) {
	    ptns[i].nr_src_netbits = nbits*8;
	    for (int j=0; j<nbits; j++)
		ptns[i].src_addr = (ptns[i].src_addr<<8)|(random()&0xff);
	} else
	    ptns[i].nr_src_netbits = 0;
	
	if (random()%3) {
	    nbits = random()%5;
	    ptns[i].nr_dst_netbits = nbits*8;
	    for (int j=0; j<nbits; j++)
		ptns[i].dst_addr = (ptns[i].dst_addr<<8)|(random()&0xff);
	} else
	    ptns[i].nr_dst_netbits = 0;
	
	if (random()%3) {
	    ptns[i].src_port = random()%(PORT_STATE_SIZE<<1);
	    if (ptns[i].src_port >= PORT_STATE_SIZE)
		ptns[i].src_port -= PORT_STATE_SIZE*2;
	} else
	    ptns[i].src_port = -1;
	
	if (random()%3) {
	    ptns[i].dst_port = random()%(PORT_STATE_SIZE<<1);
	    if (ptns[i].dst_port >= PORT_STATE_SIZE)
		ptns[i].dst_port -= PORT_STATE_SIZE*2;
	} else
	    ptns[i].dst_port = -1;
	
	if (random()%3) {
	    ptns[i].proto = random()%(PROTO_STATE_SIZE);
	} else
	    ptns[i].proto = -1;
	ptns[i].idx = i;
    }
}

int
BClassifier::initialize(ErrorHandler *errh)
{
    if (_on_cpu == 3)
	return 0;
    
    g4c_pattern_t *ptns = 0;
    int nptns = 0;
    if (_test > 2) {
	ptns = (g4c_pattern_t*)malloc(sizeof(g4c_pattern_t)*_test);
	if (!ptns) {
	    errh->error("Failed to alloc mem for patterns");
	    return -1;
	}
	memset(ptns, 0, sizeof(g4c_pattern_t)*_test);
	generate_random_patterns(ptns, _test);
	nptns = _test;
    }
    
    int s = g4c_alloc_stream();
    if (!s) {
	errh->error("Failed to alloc stream for classifier copy");
	return -1;
    }

    gcl = g4c_create_classifier(ptns, nptns, 1, s);
    if (!gcl || !gcl->devmem) {
	errh->error("Failed to create classifier");
	if (_test > 2) {
	    g4c_free_stream(s);
	    free(ptns);
	}
	return -1;
    } else {
	errh->message("Classifier built for host and device.");
	if (_test > 2)
	    free(ptns);
    }

    g4c_free_stream(s);

    if (_on_cpu < 2) {
	_batcher->setup_all();
	_anno_offset = _batcher->get_anno_offset(0);
	if (_anno_offset < 0) {
	    errh->error("Failed to get anno offset in batch "
			"anno start %u, anno len %u "
			"w start %u, w len %u",
			_batcher->anno_start, _batcher->anno_len,
			_batcher->w_anno_start, _batcher->w_anno_len);
	    return -1;
	} else
	    errh->message("BClassifier anno offset %d", _anno_offset);

	_slice_offset = _batcher->get_slice_offset(_psr);
	if (_slice_offset < 0) {
	    errh->error("Failed to get slice offset in batch ranges:");
	    for (int i=0; i<_batcher->nr_slice_ranges; i++) {
		errh->error("start %d, off %d, len %d, end %d",
			    _batcher->slice_ranges[i].start,
			    _batcher->slice_ranges[i].start_offset,
			    _batcher->slice_ranges[i].len,
			    _batcher->slice_ranges[i].end);
	    }
	    return -1;
	} else
	    errh->message("BClassifier slice offset %d", _slice_offset);
    } else {
	_anno_offset = 0;
	_slice_offset = 22; // IP dst
    }
	
    return 0;
}

void
BClassifier::bpush(int i, PBatch *p)
{
    if (!_on_cpu) {
	if (!p->dev_stream)
	    p->dev_stream = g4c_alloc_stream();
	
	g4c_gpu_classify_pkts(
	    (g4c_classifier_t*)gcl->devmem,
	    p->npkts,
	    p->dslices(),
	    p->producer->get_slice_stride(),
	    _slice_offset,
	    _slice_offset+12,
	    (int*)p->dannos(),
	    p->producer->get_anno_stride()/sizeof(int),
	    _anno_offset/sizeof(int),
	    p->dev_stream);
		
	p->hwork_ptr = p->hannos();
	p->dwork_ptr = p->dannos();
	p->work_size = p->npkts * p->producer->get_anno_stride()/sizeof(int);
    } else {
	if (_on_cpu == 3)
	    goto getout;
	
	if (_on_cpu < 2)
	    for(int j=0; j<p->npkts; j++) {
		*(int*)(g4c_ptr_add(p->anno_hptr(j), _anno_offset)) =
		    g4c_cpu_classify_pkt(
			gcl,
			(uint8_t*)g4c_ptr_add(p->slice_hptr(j),
					      _slice_offset));
	    }
	else {
	    for (int j=0; j<p->npkts; j++) {
		Packet *pkt = p->pptrs[j];
		*(int*)(g4c_ptr_add(pkt->anno(), _anno_offset)) =
		    g4c_cpu_classify_pkt(gcl, (uint8_t*)g4c_ptr_add(pkt->data(), _slice_offset));
	    }
	}
    }
getout:
    if (!_div)
	output(i).bpush(p);
    else {
	atomic_uint32_t::inc(p->shared);
	output(i*2).bpush(p);
	output(i*2+1).bpush(p);
    }	
}

void
BClassifier::push(int i, Packet *p)
{
    if (_on_cpu < 2) {
	hvp_chatter("Should never call this: %d, %p\n", i, p);
    }
    else {
	if (_on_cpu != 3) {
	    *(int*)(g4c_ptr_add(p->anno(), _anno_offset)) =
		g4c_cpu_classify_pkt(gcl, (uint8_t*)g4c_ptr_add(p->data(), _slice_offset));
	}
	output(i).push(p);
    }
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(BElement)
EXPORT_ELEMENT(BClassifier)
ELEMENT_LIBS(-lg4c)    