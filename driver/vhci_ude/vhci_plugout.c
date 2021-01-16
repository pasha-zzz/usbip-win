#include "vhci_driver.h"
#include "vhci_plugout.tmh"

#include "usbip_vhci_api.h"

static VOID
abort_pending_req_read(pctx_vusb_t vusb)
{
	WDFREQUEST	req_read_pending;

	WdfSpinLockAcquire(vusb->spin_lock);
	req_read_pending = vusb->pending_req_read;
	vusb->pending_req_read = NULL;
	WdfSpinLockRelease(vusb->spin_lock);

	if (req_read_pending != NULL) {
		TRD(PLUGIN, "abort read request");
		WdfRequestUnmarkCancelable(req_read_pending);
		WdfRequestComplete(req_read_pending, STATUS_DEVICE_NOT_CONNECTED);
	}
}

static VOID
abort_pending_urbr(purb_req_t urbr)
{
	TRD(PLUGIN, "abort pending urbr: %!URBR!", urbr);
	complete_urbr(urbr, STATUS_DEVICE_NOT_CONNECTED);
}

static VOID
abort_all_pending_urbrs(pctx_vusb_t vusb)
{
	WdfSpinLockAcquire(vusb->spin_lock);

	while (!IsListEmpty(&vusb->head_urbr)) {
		purb_req_t	urbr;

		urbr = CONTAINING_RECORD(vusb->head_urbr.Flink, urb_req_t, list_all);
		RemoveEntryListInit(&urbr->list_all);
		RemoveEntryListInit(&urbr->list_state);
		if (!unmark_cancelable_urbr(urbr))
			continue;
		WdfSpinLockRelease(vusb->spin_lock);

		abort_pending_urbr(urbr);

		WdfSpinLockAcquire(vusb->spin_lock);
	}

	WdfSpinLockRelease(vusb->spin_lock);
}

static NTSTATUS
vusb_plugout(pctx_vusb_t vusb)
{
	NTSTATUS	status;

	/*
	 * invalidate first to prevent requests from an upper layer.
	 * If requests are consistently fed into a vusb about to be plugged out,
	 * a live deadlock may occur where vusb aborts pending urbs indefinately. 
	 */
	vusb->invalid = TRUE;
	abort_pending_req_read(vusb);
	abort_all_pending_urbrs(vusb);

	status = UdecxUsbDevicePlugOutAndDelete(vusb->ude_usbdev);
	if (NT_ERROR(status)) {
		vusb->invalid = FALSE;
		TRD(PLUGIN, "failed to plug out: %!STATUS!", status);
		return status;
	}
	return STATUS_SUCCESS;
}

static NTSTATUS
plugout_all_vusbs(pctx_vhci_t vhci)
{
	ULONG	i;

	TRD(PLUGIN, "plugging out all the devices!");

	WdfSpinLockAcquire(vhci->spin_lock);
	for (i = 0; i < vhci->n_max_ports; i++) {
		NTSTATUS	status;
		pctx_vusb_t	vusb = vhci->vusbs[i];
		if (vusb == NULL)
			continue;

		vhci->vusbs[i] = VUSB_DELETING;
		WdfSpinLockRelease(vhci->spin_lock);

		status = vusb_plugout(vusb);

		WdfSpinLockAcquire(vhci->spin_lock);
		if (NT_ERROR(status)) {
			vhci->vusbs[i] = vusb;
			WdfSpinLockRelease(vhci->spin_lock);
			return STATUS_UNSUCCESSFUL;
		}
		vhci->vusbs[i] = NULL;
	}
	WdfSpinLockRelease(vhci->spin_lock);

	return STATUS_SUCCESS;
}

NTSTATUS
plugout_vusb(pctx_vhci_t vhci, ULONG port)
{
	pctx_vusb_t	vusb;
	NTSTATUS	status;

	if (port == 0)
		return plugout_all_vusbs(vhci);

	TRD(IOCTL, "plugging out device: port: %u", port);

	WdfSpinLockAcquire(vhci->spin_lock);

	vusb = vhci->vusbs[port - 1];
	if (vusb == NULL) {
		TRD(PLUGIN, "no matching vusb: port: %u", port);
		WdfSpinLockRelease(vhci->spin_lock);
		return STATUS_NO_SUCH_DEVICE;
	}

	vhci->vusbs[port - 1] = VUSB_DELETING;
	WdfSpinLockRelease(vhci->spin_lock);

	status = vusb_plugout(vusb);

	WdfSpinLockAcquire(vhci->spin_lock);
	if (NT_ERROR(status)) {
		vhci->vusbs[port - 1] = vusb;
		WdfSpinLockRelease(vhci->spin_lock);
		return STATUS_UNSUCCESSFUL;
	}
	vhci->vusbs[port - 1] = NULL;

	WdfSpinLockRelease(vhci->spin_lock);

	TRD(IOCTL, "completed to plug out: port: %u", port);

	return STATUS_SUCCESS;
}
