#define __private_include__
#include <flipper/atsam4s/atsam4s.h>
#include <os/loader.h>

/* Buffer space for incoming message runtime packets. */
struct _fmr_packet packet;

void uart0_pull_wait(void *destination, lf_size_t length) {
	/* Disable the PDC receive complete interrupt. */
	UART0->UART_IDR = UART_IDR_ENDRX;
	/* Set the transmission length and destination pointer. */
	UART0->UART_RCR = length;
	UART0->UART_RPR = (uintptr_t)(destination);
	/* Enable the receiver. */
	UART0->UART_PTCR = UART_PTCR_RXTEN;
	/* Wait until the transfer has finished. */
	while (!(UART0->UART_SR & UART_SR_ENDRX));
	/* Disable the PDC receiver. */
	UART0->UART_PTCR = UART_PTCR_RXTDIS;
	/* Enable the PDC receive complete interrupt. */
	UART0->UART_IER = UART_IER_ENDRX;
}

fmr_return fmr_push(struct _fmr_push_pull_packet *packet) {
	fmr_return retval = 0xdeadbeef;
	void *push_buffer = malloc(packet->length);
	if (!push_buffer) {
		lf_error_raise(E_MALLOC, NULL);
		return -1;
	}
	uart0_pull_wait(push_buffer, packet->length);
	if (packet->header.class == fmr_send_class) {
		/* If we are copying data, simply return a pointer to the copied data. */
		retval = (uintptr_t)push_buffer;
	} else if (packet->header.class == fmr_ram_load_class) {
		os_load_image(push_buffer);
	} else {
		*(uintptr_t *)(packet->call.parameters) = (uintptr_t)push_buffer;
		retval = fmr_execute(packet->call.index, packet->call.function, packet->call.argc, packet->call.types, (void *)(packet->call.parameters));
		free(push_buffer);
	}
	return retval;
}

fmr_return fmr_pull(struct _fmr_push_pull_packet *packet) {
	fmr_return retval = 0;
	if (packet->header.class == fmr_receive_class) {
		/* If we are receiving data, simply push the memory. */
		uart0_push((uintptr_t *)*(uint32_t *)(packet->call.parameters), packet->length);
	} else {
		void *pull_buffer = malloc(packet->length);
		if (!pull_buffer) {
			lf_error_raise(E_MALLOC, NULL);
			return -1;
		}
		*(uintptr_t *)(packet->call.parameters) = (uintptr_t)pull_buffer;
		retval = fmr_execute(packet->call.index, packet->call.function, packet->call.argc, packet->call.types, (void *)(packet->call.parameters));
		uart0_push(pull_buffer, packet->length);
		free(pull_buffer);
	}
	return retval;
}
