// Hardware interface to "USB OTG (on the go) controller" on stm32
//
// Copyright (C) 2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <string.h> // NULL
#include "autoconf.h" // CONFIG_MACH_STM32F446
#include "board/armcm_boot.h" // armcm_enable_irq
#include "board/io.h" // writel
#include "board/usb_cdc.h" // usb_notify_ep0
#include "generic/usbstd.h" // usb_string_descriptor
#include "board/usb_cdc_ep.h" // USB_CDC_EP_BULK_IN
#include "command.h" // DECL_CONSTANT_STR
#include "internal.h" // GPIO
#include "sched.h" // DECL_INIT

#define CHIP_UID_LEN            12

static void
usb_irq_disable(void)
{
    NVIC_DisableIRQ(OTG_FS_IRQn);
}

static void
usb_irq_enable(void)
{
    NVIC_EnableIRQ(OTG_FS_IRQn);
}


/****************************************************************
 * USB transfer memory
 ****************************************************************/

#define OTG ((USB_OTG_GlobalTypeDef*)USB_OTG_FS_PERIPH_BASE)
#define OTGD ((USB_OTG_DeviceTypeDef*)                          \
              (USB_OTG_FS_PERIPH_BASE + USB_OTG_DEVICE_BASE))
#define EPFIFO(EP) ((void*)(USB_OTG_FS_PERIPH_BASE + USB_OTG_FIFO_BASE  \
                            + ((EP) << 12)))
#define EPIN(EP) ((USB_OTG_INEndpointTypeDef*)                          \
                  (USB_OTG_FS_PERIPH_BASE + USB_OTG_IN_ENDPOINT_BASE    \
                   + ((EP) << 5)))
#define EPOUT(EP) ((USB_OTG_OUTEndpointTypeDef*)                        \
                   (USB_OTG_FS_PERIPH_BASE + USB_OTG_OUT_ENDPOINT_BASE  \
                    + ((EP) << 5)))

// Setup the USB fifos
static void
fifo_configure(void)
{
    // Reserve memory for Rx fifo
    uint32_t sz = ((4 * 1 + 6)
                   + 4 * ((USB_CDC_EP_BULK_OUT_SIZE / 4) + 1)
                   + (2 * 1));
    OTG->GRXFSIZ = sz;

    // Tx fifos
    uint32_t fpos = sz, ep_size = 0x10;
    OTG->DIEPTXF0_HNPTXFSIZ = ((fpos << USB_OTG_TX0FSA_Pos)
                               | (ep_size << USB_OTG_TX0FD_Pos));
    fpos += ep_size;

    OTG->DIEPTXF[USB_CDC_EP_ACM - 1] = (
        (fpos << USB_OTG_DIEPTXF_INEPTXSA_Pos)
        | (ep_size << USB_OTG_DIEPTXF_INEPTXFD_Pos));
    fpos += ep_size;

    OTG->DIEPTXF[USB_CDC_EP_BULK_IN - 1] = (
        (fpos << USB_OTG_DIEPTXF_INEPTXSA_Pos)
        | (ep_size << USB_OTG_DIEPTXF_INEPTXFD_Pos));
    fpos += ep_size;
}

// Write a packet to a tx fifo
static int_fast8_t
fifo_write_packet(uint32_t ep, const uint8_t *src, uint32_t len)
{
    void *fifo = EPFIFO(ep);
    USB_OTG_INEndpointTypeDef *epi = EPIN(ep);
    epi->DIEPINT = USB_OTG_DIEPINT_XFRC;
    epi->DIEPTSIZ = len | (1 << USB_OTG_DIEPTSIZ_PKTCNT_Pos);
    epi->DIEPCTL |= USB_OTG_DIEPCTL_EPENA | USB_OTG_DIEPCTL_CNAK;
    int32_t count = len;
    while (count >= 4) {
        uint32_t data;
        memcpy(&data, src, 4);
        writel(fifo, data);
        count -= 4;
        src += 4;
    }
    if (count) {
        uint32_t data = 0;
        memcpy(&data, src, count);
        writel(fifo, data);
    }
    return len;
}

// Read a packet from the rx queue
static int_fast8_t
fifo_read_packet(uint8_t *dest, uint_fast8_t max_len)
{
    // Transfer data
    void *fifo = EPFIFO(0);
    uint32_t grx = OTG->GRXSTSP;
    uint32_t bcnt = (grx & USB_OTG_GRXSTSP_BCNT) >> USB_OTG_GRXSTSP_BCNT_Pos;
    uint32_t xfer = bcnt > max_len ? max_len : bcnt, count = xfer;
    while (count >= 4) {
        uint32_t data = readl(fifo);
        memcpy(dest, &data, 4);
        count -= 4;
        dest += 4;
    }
    if (count) {
        uint32_t data = readl(fifo);
        memcpy(dest, &data, count);
    }
    uint32_t extra = DIV_ROUND_UP(bcnt, 4) - DIV_ROUND_UP(xfer, 4);
    while (extra--)
        readl(fifo);

    // Reenable packet reception if it got disabled by controller
    USB_OTG_OUTEndpointTypeDef *epo = EPOUT(grx & USB_OTG_GRXSTSP_EPNUM_Msk);
    uint32_t ctl = epo->DOEPCTL;
    if (!(ctl & USB_OTG_DOEPCTL_EPENA) || ctl & USB_OTG_DOEPCTL_NAKSTS) {
        epo->DOEPTSIZ = 64 | (1 << USB_OTG_DOEPTSIZ_PKTCNT_Pos);
        epo->DOEPCTL = ctl | USB_OTG_DOEPCTL_EPENA | USB_OTG_DOEPCTL_CNAK;
    }
    return xfer;
}

// Inspect the next packet on the rx queue
static uint32_t
peek_rx_queue(uint32_t ep)
{
    for (;;) {
        uint32_t sts = OTG->GINTSTS;
        if (!(sts & USB_OTG_GINTSTS_RXFLVL))
            // No packet ready
            return 0;
        uint32_t grx = OTG->GRXSTSR, grx_ep = grx & USB_OTG_GRXSTSP_EPNUM_Msk;
        uint32_t pktsts = ((grx & USB_OTG_GRXSTSP_PKTSTS_Msk)
                           >> USB_OTG_GRXSTSP_PKTSTS_Pos);
        if ((grx_ep == 0 || grx_ep == USB_CDC_EP_BULK_OUT)
            && (pktsts == 2 || pktsts == 6)) {
            // A packet is ready
            if (grx_ep != ep)
                return 0;
            return grx;
        }
        if ((grx_ep != 0 && grx_ep != USB_CDC_EP_BULK_OUT)
            || (pktsts != 1 && pktsts != 3 && pktsts != 4)) {
            // Rx queue has bogus value - just pop it
            sts = OTG->GRXSTSP;
            continue;
        }
        // Discard informational entries from queue
        fifo_read_packet(NULL, 0);
    }
}


/****************************************************************
 * USB interface
 ****************************************************************/
#define USB_STR_SERIAL_CHIPID u"0123456789ABCDEF01234567"

#define SIZE_cdc_string_serial_chipid \
    (sizeof(cdc_string_serial_chipid) + sizeof(USB_STR_SERIAL_CHIPID) - 2)

static struct usb_string_descriptor cdc_string_serial_chipid = {
    .bLength = SIZE_cdc_string_serial_chipid,
    .bDescriptorType = USB_DT_STRING,
    .data = USB_STR_SERIAL_CHIPID,
};

int_fast8_t
usb_read_bulk_out(void *data, uint_fast8_t max_len)
{
    usb_irq_disable();
    uint32_t grx = peek_rx_queue(USB_CDC_EP_BULK_OUT);
    if (!grx) {
        // Wait for packet
        OTG->GINTMSK |= USB_OTG_GINTMSK_RXFLVLM;
        usb_irq_enable();
        return -1;
    }
    int_fast8_t ret = fifo_read_packet(data, max_len);
    usb_irq_enable();
    return ret;
}

int_fast8_t
usb_send_bulk_in(void *data, uint_fast8_t len)
{
    usb_irq_disable();
    uint32_t ctl = EPIN(USB_CDC_EP_BULK_IN)->DIEPCTL;
    if (!(ctl & USB_OTG_DIEPCTL_USBAEP)) {
        // Controller not enabled - discard data
        usb_irq_enable();
        return len;
    }
    if (ctl & USB_OTG_DIEPCTL_EPENA) {
        // Wait for space to transmit
        OTGD->DAINTMSK |= 1 << USB_CDC_EP_BULK_IN;
        usb_irq_enable();
        return -1;
    }
    int_fast8_t ret = fifo_write_packet(USB_CDC_EP_BULK_IN, data, len);
    usb_irq_enable();
    return ret;
}

int_fast8_t
usb_read_ep0(void *data, uint_fast8_t max_len)
{
    usb_irq_disable();
    uint32_t grx = peek_rx_queue(0);
    if (!grx) {
        // Wait for packet
        OTG->GINTMSK |= USB_OTG_GINTMSK_RXFLVLM;
        usb_irq_enable();
        return -1;
    }
    uint32_t pktsts = ((grx & USB_OTG_GRXSTSP_PKTSTS_Msk)
                       >> USB_OTG_GRXSTSP_PKTSTS_Pos);
    if (pktsts != 2) {
        // Transfer interrupted
        usb_irq_enable();
        return -2;
    }
    int_fast8_t ret = fifo_read_packet(data, max_len);
    usb_irq_enable();
    return ret;
}

int_fast8_t
usb_read_ep0_setup(void *data, uint_fast8_t max_len)
{
    usb_irq_disable();
    for (;;) {
        uint32_t grx = peek_rx_queue(0);
        if (!grx) {
            // Wait for packet
            OTG->GINTMSK |= USB_OTG_GINTMSK_RXFLVLM;
            usb_irq_enable();
            return -1;
        }
        uint32_t pktsts = ((grx & USB_OTG_GRXSTSP_PKTSTS_Msk)
                           >> USB_OTG_GRXSTSP_PKTSTS_Pos);
        if (pktsts == 6)
            // Found a setup packet
            break;
        // Discard other packets
        fifo_read_packet(NULL, 0);
    }
    uint32_t ctl = EPIN(0)->DIEPCTL;
    if (ctl & USB_OTG_DIEPCTL_EPENA) {
        // Flush any pending tx packets
        EPIN(0)->DIEPCTL = ctl | USB_OTG_DIEPCTL_EPDIS | USB_OTG_DIEPCTL_SNAK;
        while (EPIN(0)->DIEPCTL & USB_OTG_DIEPCTL_EPENA)
            ;
        OTG->GRSTCTL = USB_OTG_GRSTCTL_TXFFLSH;
        while (OTG->GRSTCTL & USB_OTG_GRSTCTL_TXFFLSH)
            ;
    }
    int_fast8_t ret = fifo_read_packet(data, max_len);
    usb_irq_enable();
    return ret;
}

int_fast8_t
usb_send_ep0(const void *data, uint_fast8_t len)
{
    usb_irq_disable();
    uint32_t grx = peek_rx_queue(0);
    if (grx) {
        // Transfer interrupted
        usb_irq_enable();
        return -2;
    }
    if (EPIN(0)->DIEPCTL & USB_OTG_DIEPCTL_EPENA) {
        // Wait for space to transmit
        OTG->GINTMSK |= USB_OTG_GINTMSK_RXFLVLM;
        OTGD->DAINTMSK |= 1 << 0;
        usb_irq_enable();
        return -1;
    }
    int_fast8_t ret = fifo_write_packet(0, data, len);
    usb_irq_enable();
    return ret;
}

void
usb_stall_ep0(void)
{
    usb_irq_disable();
    EPIN(0)->DIEPCTL |= USB_OTG_DIEPCTL_STALL;
    usb_notify_ep0(); // XXX - wake from main usb_cdc.c code?
    usb_irq_enable();
}

void
usb_set_address(uint_fast8_t addr)
{
    OTGD->DCFG = ((OTGD->DCFG & ~USB_OTG_DCFG_DAD_Msk)
                  | (addr << USB_OTG_DCFG_DAD_Pos));
    usb_send_ep0(NULL, 0);
    usb_notify_ep0();
}

void
usb_set_configure(void)
{
    usb_irq_disable();
    // Configure and enable USB_CDC_EP_ACM
    USB_OTG_INEndpointTypeDef *epi = EPIN(USB_CDC_EP_ACM);
    epi->DIEPTSIZ = (USB_CDC_EP_ACM_SIZE
                     | (1 << USB_OTG_DIEPTSIZ_PKTCNT_Pos));
    epi->DIEPCTL = (
        USB_OTG_DIEPCTL_SNAK | USB_OTG_DIEPCTL_USBAEP
        | (0x03 << USB_OTG_DIEPCTL_EPTYP_Pos) | USB_OTG_DIEPCTL_SD0PID_SEVNFRM
        | (USB_CDC_EP_ACM << USB_OTG_DIEPCTL_TXFNUM_Pos)
        | (USB_CDC_EP_ACM_SIZE << USB_OTG_DIEPCTL_MPSIZ_Pos));

    // Configure and enable USB_CDC_EP_BULK_OUT
    USB_OTG_OUTEndpointTypeDef *epo = EPOUT(USB_CDC_EP_BULK_OUT);
    epo->DOEPTSIZ = 64 | (1 << USB_OTG_DOEPTSIZ_PKTCNT_Pos);
    epo->DOEPCTL = (
        USB_OTG_DOEPCTL_CNAK | USB_OTG_DOEPCTL_USBAEP | USB_OTG_DOEPCTL_EPENA
        | (0x02 << USB_OTG_DOEPCTL_EPTYP_Pos) | USB_OTG_DOEPCTL_SD0PID_SEVNFRM
        | (USB_CDC_EP_BULK_OUT_SIZE << USB_OTG_DOEPCTL_MPSIZ_Pos));

    // Configure and flush USB_CDC_EP_BULK_IN
    epi = EPIN(USB_CDC_EP_BULK_IN);
    epi->DIEPTSIZ = (USB_CDC_EP_BULK_IN_SIZE
                     | (1 << USB_OTG_DIEPTSIZ_PKTCNT_Pos));
    epi->DIEPCTL = (
        USB_OTG_DIEPCTL_SNAK | USB_OTG_DIEPCTL_EPDIS | USB_OTG_DIEPCTL_USBAEP
        | (0x02 << USB_OTG_DIEPCTL_EPTYP_Pos) | USB_OTG_DIEPCTL_SD0PID_SEVNFRM
        | (USB_CDC_EP_BULK_IN << USB_OTG_DIEPCTL_TXFNUM_Pos)
        | (USB_CDC_EP_BULK_IN_SIZE << USB_OTG_DIEPCTL_MPSIZ_Pos));
    while (epi->DIEPCTL & USB_OTG_DIEPCTL_EPENA)
        ;
    OTG->GRSTCTL = ((USB_CDC_EP_BULK_IN << USB_OTG_GRSTCTL_TXFNUM_Pos)
                    | USB_OTG_GRSTCTL_TXFFLSH);
    while (OTG->GRSTCTL & USB_OTG_GRSTCTL_TXFFLSH)
        ;
    usb_irq_enable();
}

struct usb_string_descriptor *
usbserial_get_serialid(void)
{
   return &cdc_string_serial_chipid;
}

/****************************************************************
 * Setup and interrupts
 ****************************************************************/
static void
usb_set_serial(void)
{
    uint8_t *chipid = (uint8_t *)UID_BASE;
    uint8_t i, j, c;
    for (i = 0; i < CHIP_UID_LEN; i++) {
        for (j = 0; j < 2; j++) {
            c = (chipid[i] >> 4*j) & 0xF;
            c = (c < 10) ? '0'+c : 'A'-10+c;
            cdc_string_serial_chipid.data[2*i+((j)?0:1)] = c;
        }
    }
}

// Main irq handler
void
OTG_FS_IRQHandler(void)
{
    uint32_t sts = OTG->GINTSTS;
    if (sts & USB_OTG_GINTSTS_RXFLVL) {
        // Received data - disable irq and notify endpoint
        OTG->GINTMSK &= ~USB_OTG_GINTMSK_RXFLVLM;
        uint32_t grx = OTG->GRXSTSR, ep = grx & USB_OTG_GRXSTSP_EPNUM_Msk;
        if (ep == 0)
            usb_notify_ep0();
        else
            usb_notify_bulk_out();
    }
    if (sts & USB_OTG_GINTSTS_IEPINT) {
        // Can transmit data - disable irq and notify endpoint
        uint32_t daint = OTGD->DAINT;
        OTGD->DAINTMSK &= ~daint;
        if (daint & (1 << 0))
            usb_notify_ep0();
        if (daint & (1 << USB_CDC_EP_BULK_IN))
            usb_notify_bulk_in();
    }
}

DECL_CONSTANT_STR("RESERVE_PINS_USB", "PA11,PA12");

// Initialize the usb controller
void
usb_init(void)
{
    if (CONFIG_USB_SERIAL_NUMBER_CHIPID)
        usb_set_serial();

    // Enable USB clock
    RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
    while (!(OTG->GRSTCTL & USB_OTG_GRSTCTL_AHBIDL))
        ;

    // Configure USB in full-speed device mode
    OTG->GUSBCFG = (USB_OTG_GUSBCFG_FDMOD | USB_OTG_GUSBCFG_PHYSEL
                    | (6 << USB_OTG_GUSBCFG_TRDT_Pos));
    OTGD->DCFG |= (3 << USB_OTG_DCFG_DSPD_Pos);
#if CONFIG_MACH_STM32F446
    OTG->GOTGCTL = USB_OTG_GOTGCTL_BVALOEN | USB_OTG_GOTGCTL_BVALOVAL;
#else
    OTG->GCCFG |= USB_OTG_GCCFG_NOVBUSSENS;
#endif

    // Route pins
    gpio_peripheral(GPIO('A', 11), GPIO_FUNCTION(10), 0);
    gpio_peripheral(GPIO('A', 12), GPIO_FUNCTION(10), 0);

    // Setup USB packet memory
    fifo_configure();

    // Configure and enable ep0
    uint32_t mpsize_ep0 = 2;
    USB_OTG_INEndpointTypeDef *epi = EPIN(0);
    USB_OTG_OUTEndpointTypeDef *epo = EPOUT(0);
    epi->DIEPCTL = mpsize_ep0 | USB_OTG_DIEPCTL_SNAK;
    epo->DOEPTSIZ = (64 | (1 << USB_OTG_DOEPTSIZ_STUPCNT_Pos)
                     | (1 << USB_OTG_DOEPTSIZ_PKTCNT_Pos));
    epo->DOEPCTL = mpsize_ep0 | USB_OTG_DOEPCTL_EPENA | USB_OTG_DOEPCTL_CNAK;

    // Enable interrupts
    OTGD->DIEPMSK = USB_OTG_DIEPMSK_XFRCM;
    OTG->GINTMSK = USB_OTG_GINTMSK_RXFLVLM | USB_OTG_GINTMSK_IEPINT;
    OTG->GAHBCFG = USB_OTG_GAHBCFG_GINT;
    armcm_enable_irq(OTG_FS_IRQHandler, OTG_FS_IRQn, 1);

    // Enable USB
    OTG->GCCFG |= USB_OTG_GCCFG_PWRDWN;
    OTGD->DCTL = 0;
}
DECL_INIT(usb_init);
