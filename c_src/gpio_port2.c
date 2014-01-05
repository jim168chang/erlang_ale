/**
* @file   gpio_port.c
* @author Erlang Solutions Ltd
* @brief  GPIO erlang interface
* @description
*
* @section LICENSE
* Copyright (C) 2013 Erlang Solutions Ltd.
**/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <err.h>

#include <erl_interface.h>
#include <ei.h>

#define BUF_SIZE 1024

/*! \addtogroup GPIO
*  @brief GPIO library functions
*  @{
*/

enum gpio_state {
    GPIO_CLOSED,
    GPIO_OUTPUT,
    GPIO_INPUT,
    GPIO_INPUT_WITH_INTERRUPTS
};

struct gpio {
    enum gpio_state state;
    int fd;
    int pin_number;
};

int gpio_open(struct gpio *pin, unsigned int pin_number, const char *dir);
int gpio_release(struct gpio *pin);

void erlcmd_send(ETERM *response);

/**
 * @brief write a string to a sysfs file
 * @return returns 0 on failure, >0 on success
 */
int sysfs_write_file(const char *pathname, const char *value)
{
    int fd = open(pathname, O_WRONLY);
    if (fd < 0) {
	warn("Error opening %s", pathname);
	return 0;
    }

    size_t count = strlen(value);
    ssize_t written = write(fd, value, count);
    close(fd);

    if (written != count) {
	warn("Error writing '%s' to %s", value, pathname);
	return 0;
    }

    return written;
}


// GPIO functions

void gpio_init(struct gpio *pin)
{
    pin->state = GPIO_CLOSED;
    pin->fd = -1;
    pin->pin_number = -1;
}

/**
 * @brief	Initialises the devname GPIO device
 *
 * @param	pin_number            The GPIO pin
 * @param       dir                   Direction of pin (input or output)
 *
 * @return 	1 for success, -1 for failure
 */
int gpio_open(struct gpio *pin, unsigned int pin_number, const char *dir)
{
    /* If not closed, then release whatever pin is currently open. */
    if (pin->state != GPIO_CLOSED)
	gpio_release(pin);

    /* Check if the gpio has been exported already. */
    char path[64];
    sprintf(path, "/sys/class/gpio/gpio%d/direction", pin_number);
    if (access(path, F_OK) == -1) {
	/* Nope. Export it. */
	char pinstr[64];
	sprintf(pinstr, "%d", pin_number);
	if (!sysfs_write_file("/sys/class/gpio/export", pinstr))
	    return -1;
    }

    const char *dirstr;
    if (strcmp(dir, "input") == 0) {
	dirstr = "in";
	pin->state = GPIO_INPUT;
    } else if (strcmp(dir, "output") == 0) {
	dirstr = "out";
	pin->state = GPIO_OUTPUT;
    } else
	return 1;

    if (!sysfs_write_file(path, dirstr))
	return 1;

    pin->pin_number = pin_number;

    /* Open the value file for quick access later */
    sprintf(path, "/sys/class/gpio/gpio%d/value", pin_number);
    pin->fd = open(path, pin->state == GPIO_OUTPUT ? O_RDWR : O_RDONLY);

    return 1;
}

/**
 * @brief	Release a GPIO pin
 *
 * @param	pin            The GPIO pin
 *
 * @return 	1 for success, -1 for failure
 */
int gpio_release(struct gpio *pin)
{
    if (pin->state == GPIO_CLOSED)
	return 1;

    /* Close down the value file */
    close(pin->fd);
    pin->fd = -1;

    /* Unexport the pin */
    char pinstr[64];
    sprintf(pinstr, "%d", pin->pin_number);
    sysfs_write_file("/sys/class/gpio/unexport", pinstr);

    pin->state = GPIO_CLOSED;
    pin->pin_number = -1;
    return 1;
}

/**
 * @brief	Set pin with the value "0" or "1"
 *
 * @param	pin            The GPIO pin
 * @param       value         Value to set (0 or 1)
 *
 * @return 	1 for success, -1 for failure
 */
int gpio_write(struct gpio *pin, unsigned int val)
{
    if (pin->state != GPIO_OUTPUT)
	return -1;

    char buf = val ? '1' : '0';
    ssize_t amount_written = pwrite(pin->fd, &buf, sizeof(buf), 0);
    if (amount_written < sizeof(buf))
	err(EXIT_FAILURE, "pwrite");

    return 1;
}

/**
* @brief	Read the value of the pin
*
* @param	pin            The GPIO pin
*
* @return 	The pin value if success, -1 for failure
*/
int gpio_read(struct gpio *pin)
{
    if (pin->state == GPIO_CLOSED)
	return -1;

    char buf;
    ssize_t amount_read = pread(pin->fd, &buf, sizeof(buf), 0);
    if (amount_read < sizeof(buf))
	err(EXIT_FAILURE, "pread");

    return buf == '1' ? 1 : 0;
}

/**
 * Set isr as the interrupt service routine (ISR) for the pin. Mode
 * should be one of the strings "rising", "falling" or "both" to
 * indicate which edge(s) the ISR is to be triggered on. The function
 * isr is called whenever the edge specified occurs, receiving as
 * argument the number of the pin which triggered the interrupt.
 *
 * @param   pin	Pin number to attach interrupt to
 * @param   mode	Interrupt mode
 *
 * @return  Returns 1 on success.
 */
int gpio_set_int(struct gpio *pin, const char *mode)
{
    char path[64];
    sprintf(path, "/sys/class/gpio/gpio%d/edge", pin->pin_number);
    if (!sysfs_write_file(path, mode))
	return -1;

    pin->state = GPIO_INPUT_WITH_INTERRUPTS;
    return 1;
}

void gpio_process(struct gpio *pin)
{
    int value = gpio_read(pin);

    ETERM *resp;
    if (value)
	resp = erl_format("{gpio_interrupt, rising}");
    else
	resp = erl_format("{gpio_interrupt, falling}");

    erlcmd_send(resp);
    erl_free_term(resp);
}

struct erlcmd
{
    unsigned char buffer[BUF_SIZE];
    ssize_t index;
};

void erlcmd_init(struct erlcmd *handler)
{
    erl_init(NULL, 0);
    memset(handler, 0, sizeof(*handler));
}

/**
 * @brief Synchronously send a response back to Erlang
 */
void erlcmd_send(ETERM *response)
{
    unsigned char buf[1024];

    if (erl_encode(response, buf + sizeof(uint16_t)) == 0)
	errx(EXIT_FAILURE, "erl_encode");

    ssize_t len = erl_term_len(response);
    uint16_t be_len = htons(len);
    memcpy(buf, &be_len, sizeof(be_len));

    len += sizeof(uint16_t);
    ssize_t wrote = 0;
    do {
	ssize_t amount_written = write(STDOUT_FILENO, buf + wrote, len - wrote);
	if (amount_written < 0) {
	    if (errno == EINTR)
		continue;

	    err(EXIT_FAILURE, "write");
	}

	wrote += amount_written;
    } while (wrote < len);
}

/**
 * @brief Dispatch commands in the buffer
 * @return the number of bytes processed
 */
ssize_t erlcmd_dispatch(struct erlcmd *handler, struct gpio *pin)
{
    /* Check for length field */
    if (handler->index < sizeof(uint16_t))
	return 0;

    uint16_t be_len;
    memcpy(&be_len, handler->buffer, sizeof(uint16_t));
    ssize_t msglen = ntohs(be_len);
    if (msglen + sizeof(uint16_t) > sizeof(handler->buffer))
	errx(EXIT_FAILURE, "Message too long");
    else if (msglen + sizeof(uint16_t) < handler->index)
	return 0;

    ETERM *emsg = erl_decode(handler->buffer + sizeof(uint16_t));
    if (emsg == NULL)
	errx(EXIT_FAILURE, "erl_decode");

    ETERM *msg_type = erl_element(1, emsg);
    if (msg_type == NULL)
	errx(EXIT_FAILURE, "erl_element(msg_type)");

    if (strcmp(ERL_ATOM_PTR(msg_type), "init") == 0) {
	ETERM *arg1p = erl_element(2, emsg);
	ETERM *arg2p = erl_element(3, emsg);
	if (arg1p == NULL || arg2p == NULL)
	    errx(EXIT_FAILURE, "init: arg1p or arg2p was NULL");

	/* convert erlang terms to usable values */
	int pin_number = ERL_INT_VALUE(arg1p);

	ETERM *resp;
	if (gpio_open(pin, pin_number, ERL_ATOM_PTR(arg2p)))
	    resp = erl_format("ok");
	else
	    resp = erl_format("{error, gpio_init_fail}");

	erlcmd_send(resp);
	erl_free_term(arg1p);
	erl_free_term(arg2p);
	erl_free_term(resp);
    } else if (strcmp(ERL_ATOM_PTR(msg_type), "cast") == 0) {
	ETERM *arg1p = erl_element(2, emsg);
	if (arg1p == NULL)
	    errx(EXIT_FAILURE, "cast: arg1p was NULL");

	if (strcmp(ERL_ATOM_PTR(arg1p), "release") == 0) {
	    gpio_release(pin);
	} else
	    errx(EXIT_FAILURE, "cast: bad command");

	erl_free_term(arg1p);
    } else if (strcmp(ERL_ATOM_PTR(msg_type), "call") == 0) {
	ETERM *refp = erl_element(2, emsg);
        ETERM *tuplep = erl_element(3, emsg);
	if (refp == NULL || tuplep == NULL)
	    errx(EXIT_FAILURE, "call: refp or tuplep was NULL");

	ETERM *fnp = erl_element(1, tuplep);
	if (fnp == NULL)
	    errx(EXIT_FAILURE, "tuplep: fnp was NULL");

	ETERM *resp = 0;
	if (strcmp(ERL_ATOM_PTR(fnp), "write") == 0) {
	    ETERM *arg1p = erl_element(2, tuplep);
	    if (arg1p == NULL)
		errx(EXIT_FAILURE, "write: arg1p was NULL");

	    int value = ERL_INT_VALUE(arg1p);
	    if(gpio_write(pin, value))
		resp = erl_format("ok");
	    else
		resp = erl_format("{error, gpio_write_failed}");
	    erl_free_term(arg1p);
	} else if (strcmp(ERL_ATOM_PTR(fnp), "read") == 0) {
	    int value = gpio_read(pin);
	    if (value !=-1)
		resp = erl_format("~i", value);
	    else
		resp = erl_format("{error, gpio_read_failed}");
	} else if (strcmp(ERL_ATOM_PTR(fnp), "set_int") == 0) {
	    ETERM *arg1p = erl_element(2, tuplep);

	    if (gpio_set_int(pin, ERL_ATOM_PTR(arg1p)))
		resp = erl_format("ok");
	    else
		resp = erl_format("{error, gpio_set_int_failed}");
	    erl_free_term(arg1p);
	}

	ETERM *fullresp = erl_format("{port_reply,~w,~w}", refp, resp);
	erlcmd_send(fullresp);

	erl_free_term(fullresp);
	erl_free_term(resp);
	erl_free_term(fnp);
	erl_free_term(tuplep);
	erl_free_term(refp);
     } else {
	errx(EXIT_FAILURE, "unexpected element");
     }

     erl_free_term(emsg);
     erl_free_term(msg_type);

     return msglen + sizeof(uint16_t);
}

/**
 * @brief call to process any new requests from Erlang
 */
void erlcmd_process(struct erlcmd *handler, struct gpio *pin)
{
    ssize_t amount_read = read(STDIN_FILENO, handler->buffer, sizeof(handler->buffer) - handler->index);
    if (amount_read < 0) {
	/* EINTR is ok to get, since we were interrupted by a signal. */
	if (errno == EINTR) {
	    return;
	}

	/* Everything else is unexpected. */
	err(EXIT_FAILURE, "read");
    } else if (amount_read == 0) {
	/* EOF. Erlang process was terminated. This happens after a release or if there was an error. */
	exit(EXIT_SUCCESS);
    }

    handler->index += amount_read;
    for (;;) {
	ssize_t bytes_processed = erlcmd_dispatch(handler, pin);

	if (bytes_processed == 0) {
	    /* Only have part of the command to process. */
	    break;
	} else if (handler->index > bytes_processed) {
	    /* Processed the command and there's more data. */
	    memmove(handler->buffer, &handler->buffer[bytes_processed], handler->index - bytes_processed);
	    handler->index -= bytes_processed;
	} else {
	    /* Processed the whole buffer. */
	    handler->index = 0;
	    break;
	}
    }
}

/**
 * @brief The main function.
 * It waits for data in the buffer and calls the driver.
 */
int main()
{
    struct gpio pin;
    struct erlcmd handler;

    erlcmd_init(&handler);
    gpio_init(&pin);

    for (;;) {
	struct pollfd fdset[2];

	fdset[0].fd = STDIN_FILENO;
	fdset[0].events = POLLIN;
	fdset[0].revents = 0;

	fdset[1].fd = pin.fd;
	fdset[1].events = POLLPRI;
	fdset[1].revents = 0;

	int rc = poll(fdset, pin.state == GPIO_INPUT_WITH_INTERRUPTS ? 2 : 1, -1);
	if (rc < 0) {
	    /* Retry if EINTR */
	    if (errno == EINTR)
		continue;

	    err(EXIT_FAILURE, "poll");
	}

	if (rc && (fdset[0].revents & (POLLIN | POLLHUP))) {
	    erlcmd_process(&handler, &pin);
	    rc--;
	}

	if (rc && (fdset[1].revents & POLLPRI)) {
	    gpio_process(&pin);
	    rc--;
	}

	/* For debugging only. */
	if (rc != 0)
	    errx(EXIT_FAILURE, "Unexpected return from poll(). rc=%d, revents(0)=0x%04x, revents(1)=0x%04x",
		 rc, fdset[0].revents, fdset[1].revents);
    }

    return 0;
}