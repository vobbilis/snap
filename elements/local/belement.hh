#ifndef CLICK_BELEMENT_HH
#define CLICK_BELEMENT_HH
#include <click/glue.hh>
#include <click/element.hh>
#include <click/pbatch.hh>
CLICK_DECLS

class BElement : public Element {
public:
    BElement();
    virtual ~BElement();
    
    virtual const char *class_name() const	{ return "BElement"; }
    virtual const char *port_count() const	{ return "*/*"; }
    virtual const char *processing() const  { return AGNOSTIC; }

    virtual PBatch* bpull(int port);
    virtual void bpush(int i, PBatch *p);

    // modes of BElements, mostly for testing.
    enum {
	ON_NORMAL = 0,
	CPU_BATCH = 1,
	CPU_NOBATCH = 2,
	ONNONE = 3 };
};

CLICK_ENDDECLS
#endif