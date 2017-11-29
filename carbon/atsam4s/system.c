#define __private_include__
#include <osmium.h>
#include <scheduler.h>
#include <loader.h>
#include <flipper/atsam4s/atsam4s.h>

/* Buffer space for incoming message runtime packets. */
struct _fmr_packet packet;

void uart0_pull_wait(void *destination, lf_size_t length) {
	/* Disable the PDC receive complete interrupt. */
	UART0 -> UART_IDR = UART_IDR_ENDRX;
	/* Set the transmission length and destination pointer. */
	UART0 -> UART_RCR = length;
	UART0 -> UART_RPR = (uintptr_t)(destination);
	/* Enable the receiver. */
	UART0 -> UART_PTCR = UART_PTCR_RXTEN;
	/* Wait until the transfer has finished. */
	while (!(UART0 -> UART_SR & UART_SR_ENDRX));
	/* Disable the PDC receiver. */
	UART0 -> UART_PTCR = UART_PTCR_RXTDIS;
	/* Enable the PDC receive complete interrupt. */
	UART0 -> UART_IER = UART_IER_ENDRX;
}

fmr_return fmr_push(struct _fmr_push_pull_packet *packet) {
	fmr_return retval = 0;
	void *push_buffer = malloc(packet -> length);
	if (!push_buffer) {
		lf_error_raise(E_MALLOC, NULL);
		return -1;
	}
	uart0_pull_wait(push_buffer, packet -> length);
	if (packet -> header.class == fmr_send_class) {
		/* If we are copying data, simply return a pointer to the copied data. */
		retval = (uintptr_t)push_buffer;
	} else if (packet -> header.class == fmr_ram_load_class) {
		retval = os_load_image(push_buffer);
	} else {
		*(uintptr_t *)(packet -> call.parameters) = (uintptr_t)push_buffer;
		retval = fmr_execute(packet -> call.index, packet -> call.function, packet -> call.argc, packet -> call.types, (void *)(packet -> call.parameters));
		free(push_buffer);
	}
	return retval;
}

fmr_return fmr_pull(struct _fmr_push_pull_packet *packet) {
	fmr_return retval = 0;
	if (packet -> header.class == fmr_receive_class) {
		/* If we are receiving data, simply push the memory. */
		uart0_push((uintptr_t *)*(uint32_t *)(packet -> call.parameters), packet -> length);
	} else {
		void *pull_buffer = malloc(packet -> length);
		if (!pull_buffer) {
			lf_error_raise(E_MALLOC, NULL);
			return -1;
		}
		*(uintptr_t *)(packet -> call.parameters) = (uintptr_t)pull_buffer;
		retval = fmr_execute(packet -> call.index, packet -> call.function, packet -> call.argc, packet -> call.types, (void *)(packet -> call.parameters));
		uart0_push(pull_buffer, packet -> length);
		free(pull_buffer);
	}
	return retval;
}

/* System task is executed alongside user tasks. */
void system_task(void) {

	/* -------- SYSTEM TASK -------- */

	/* Nothing to do here, so move on to the next task. */
	os_task_next();
}

void uart0_isr(void) {
	/* If an entire packet has been received, process it. */
	if (UART0 -> UART_SR & UART_SR_ENDRX) {
		/* Disable the PDC receiver. */
		UART0 -> UART_PTCR = UART_PTCR_RXTDIS;
		/* Clear the PDC RX interrupt flag. */
		UART0 -> UART_RCR = 1;
		/* Create a result. */
		struct _fmr_result result = { 0 };
		/* Process the packet. */
		fmr_perform(&packet, &result);
		/* HACK: Wait until the U2 is ready. */
		for (volatile int i = 0; i < 10000; i ++);
		/* Give the result back. */
		uart0_push(&result, sizeof(struct _fmr_result));
		/* Flush any remaining data that has been buffered. */
		while (UART0 -> UART_SR & UART_SR_RXRDY) UART0 -> UART_RHR;
		/* Clear the error state. */
		lf_error_clear();
		/* Pull the next packet asynchronously. */
		uart0_pull(&packet, sizeof(struct _fmr_packet));
	}
}

/* Called once on system initialization. */
void system_init(void) {

	/* Disable the watchdog timer. */
	WDT -> WDT_MR = WDT_MR_WDDIS;

	/* Configure the EFC for 5 wait states. */
	EFC -> EEFC_FMR = EEFC_FMR_FWS(PLATFORM_WAIT_STATES);

	/* Configure the primary clock source. */
	if (!(PMC -> CKGR_MOR & CKGR_MOR_MOSCSEL)) {
		PMC -> CKGR_MOR = CKGR_MOR_KEY(0x37) | BOARD_OSCOUNT | CKGR_MOR_MOSCRCEN | CKGR_MOR_MOSCXTEN;
		for (uint32_t timeout = 0; !(PMC -> PMC_SR & PMC_SR_MOSCXTS) && (timeout ++ < CLOCK_TIMEOUT););
	}

	/* Select external 20MHz oscillator. */
	PMC -> CKGR_MOR = CKGR_MOR_KEY(0x37) | BOARD_OSCOUNT | CKGR_MOR_MOSCRCEN | CKGR_MOR_MOSCXTEN | CKGR_MOR_MOSCSEL;
	for (uint32_t timeout = 0; !(PMC -> PMC_SR & PMC_SR_MOSCSELS) && (timeout ++ < CLOCK_TIMEOUT););
	PMC -> PMC_MCKR = (PMC -> PMC_MCKR & ~(uint32_t)PMC_MCKR_CSS_Msk) | PMC_MCKR_CSS_MAIN_CLK;
	for (uint32_t timeout = 0; !(PMC -> PMC_SR & PMC_SR_MCKRDY) && (timeout++ < CLOCK_TIMEOUT););

	/* Configure PLLB as the master clock PLL. */
	PMC -> CKGR_PLLBR = BOARD_PLLBR;
	for (uint32_t timeout = 0; !(PMC -> PMC_SR & PMC_SR_LOCKB) && (timeout++ < CLOCK_TIMEOUT););

	/* Switch to the main clock. */
	PMC -> PMC_MCKR = (BOARD_MCKR & ~PMC_MCKR_CSS_Msk) | PMC_MCKR_CSS_MAIN_CLK;
	for (uint32_t timeout = 0; !(PMC -> PMC_SR & PMC_SR_MCKRDY) && (timeout++ < CLOCK_TIMEOUT););
	PMC -> PMC_MCKR = BOARD_MCKR;
	for (uint32_t timeout = 0; !(PMC -> PMC_SR & PMC_SR_MCKRDY) && (timeout++ < CLOCK_TIMEOUT););

	/* Allow the reset pin to reset the device. */
	RSTC -> RSTC_MR = RSTC_MR_KEY(0xA5) | RSTC_MR_URSTEN;

	/* Configure the USART peripheral. */
	usart_configure();
	/* Configure the UART peripheral. */
	uart0_configure(NULL);
	/* Configure the GPIO peripheral. */
	gpio_configure();
	/* Configure the SPI peripheral. */
	spi_configure();

	/* Pull an FMR packet asynchronously to launch FMR. */
	uart0_pull(&packet, sizeof(struct _fmr_packet));
	/* Enable the PDC receive complete interrupt. */
	UART0 -> UART_IER = UART_IER_ENDRX;

	/* Print reset message. */
	char reset_msg[] = "Reset\n";
	usart_push(reset_msg, sizeof(reset_msg) - 1);

}

void system_deinit(void) {

}
