/***************************************************************************************************
 * Author : Rishi Gupta
 * Email  : gupt21@gmail.com
 *
 * This file is part of 'serial communication manager' library.
 *
 * The 'serial communication manager' is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * The 'serial communication manager' is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with serial communication manager. If not, see <http://www.gnu.org/licenses/>.
 *
 ***************************************************************************************************/

/* We have tried to follow the philosophy that resources specific to thread should be held by thread
 * and that the thread is responsible for cleaning them before exiting. */

#if defined (__linux__) || defined (__APPLE__) || defined (__SunOS)

#include <unistd.h>     	/* UNIX standard function definitions */
#include <stdio.h>
#include <stdlib.h>     	/* Standard ANSI routines             */
#include <string.h>     	/* String function definitions        */
#include <fcntl.h>      	/* File control definitions           */
#include <errno.h>      	/* Error number definitions           */
#include <dirent.h>     	/* Format of directory entries        */
#include <sys/types.h>  	/* Primitive System Data Types        */
#include <sys/stat.h>   	/* Defines the structure of the data  */
#include <pthread.h>		/* POSIX thread definitions	          */
#include <sys/select.h>

#if defined (__linux__)
#include <linux/types.h>
#include <linux/termios.h>  /* POSIX terminal control definitions for Linux (termios2) */
#include <linux/serial.h>
#include <linux/ioctl.h>
#include <sys/eventfd.h>    /* Linux eventfd for event notification. */
#include <sys/epoll.h>      /* epoll feature of Linux	              */
#include <signal.h>
#endif

#if defined (__APPLE__)
#include <termios.h>
#include <sys/ioctl.h>
#include <paths.h>
#include <sysexits.h>
#include <sys/param.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#include <IOKit/serial/ioss.h>
#include <IOKit/IOBSD.h>
#include <sys/event.h>
#endif

#if defined (__SunOS)
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#endif

#include <jni.h>
#include "unix_like_serial_lib.h"

#define DEBUG 1

/* Do not let any exception propagate. Handle and clear it. */
void LOGE(JNIEnv *env) {
	(*env)->ExceptionDescribe(env);
	(*env)->ExceptionClear(env);
}

/* pselect() is used to provide delay whenever required. */
int serial_delay(unsigned milliSeconds) {
	struct timespec t;
	t.tv_sec  = milliSeconds/1000;
	t.tv_nsec = 0;
	pselect(1, 0, 0, 0, &t, 0);
	return 0;
}

/* This handler is invoked whenever application unregisters event listener. */
void exit_signal_handler(int signal_number) {
	if(signal_number == SIGUSR1) {
		if(DEBUG) fprintf(stderr, "%s\n", "SIGUSR1 RECEIVED");
		if(DEBUG) fflush(stderr);
		pthread_exit((void *)0);
	}else {
		if(DEBUG) fprintf(stderr, "%s %d\n", "UNKNOWN SIGNAL", signal_number);
		if(DEBUG) fflush(stderr);
	}
}

/* This thread wait for data to be available on fd and enqueues it in data queue managed by java layer.
 * For unrecoverable errors thread would like to exit and try again. */
void *data_looper(void *arg) {
	int i = -1;
	int negative = -1;
	int index = 0;
	int partialData = -1;
	ssize_t ret = -1;
	jbyte buffer[1024];
	jbyte final_buf[1024 * 3]; 	  /* Sufficient enough to deal with consecutive multiple partial reads. */
	jbyte empty_buf[] = { };
	jbyteArray dataRead;

#if defined (__linux__)
	/* Epoll is used for Linux systems.
	 * ev_port refers to serial port fd and ev_exit refers to fd on which we make an event happen explicitly
	 * so as to signal epoll_wait come out of waiting state. */
	int MAXEVENTS = 4;
	int epfd = 0;
	int evfd = 0;
	struct epoll_event ev_port;
	struct epoll_event ev_exit;
	struct epoll_event event;
	struct epoll_event *events = calloc(MAXEVENTS, sizeof(event));
#endif
#if defined (__APPLE__)
	/* Kqueue is used for MAC OS X systems. */
	int kq;
	struct kevent chlist[2];   /* events to monitor */
	struct kevent evlist[2];   /* events that were triggered */
	int pipe1[2]; /* pipe1[0] is reading end, and pipe1[1] is writing end. */
#endif

	pthread_mutex_lock(((struct com_thread_params*) arg)->mutex);

	struct com_thread_params* params = (struct com_thread_params*) arg;
	JavaVM *jvm = (*params).jvm;
	int fd = (*params).fd;
	jobject looper = (*params).looper;

	/* The JNIEnv is valid only in the current thread. So, threads created should attach itself to the VM and obtain a JNI interface pointer. */
	void* env1;
	JNIEnv* env;
	if((*jvm)->AttachCurrentThread(jvm, &env1, NULL) != JNI_OK) {
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread failed to attach itself to JVM to access JNI ENV !.");
		if(DEBUG) fflush(stderr);
		((struct com_thread_params*) arg)->data_thread_id = 0;
		((struct com_thread_params*) arg)->data_init_done = -240;
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
	}

	env = (JNIEnv*) env1;
	jclass SerialComLooper = (*env)->GetObjectClass(env, looper);
	if(SerialComLooper == NULL) {
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread could not get class of object of type looper !");
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread exiting. Please RETRY registering data listener !");
		if(DEBUG) fflush(stderr);
		((struct com_thread_params*) arg)->data_thread_id = 0;
		((struct com_thread_params*) arg)->data_init_done = -240;
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
	}

	jmethodID mid = (*env)->GetMethodID(env, SerialComLooper, "insertInDataQueue", "([B)V");
	if((*env)->ExceptionOccurred(env)) {
		LOGE(env);
	}
	if(mid == NULL) {
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread failed to retrieve method id of method insertInDataQueue in class SerialComLooper !");
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread exiting. Please RETRY registering data listener !");
		if(DEBUG) fflush(stderr);
		((struct com_thread_params*) arg)->data_thread_id = 0;
		((struct com_thread_params*) arg)->data_init_done = -240;
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
	}

#if defined (__linux__)
	errno = 0;
	ret  = eventfd(0, O_NONBLOCK);
	if(ret < 0) {
		if(DEBUG) fprintf(stderr, "%s %d\n", "NATIVE data_looper() thread failed to create eventfd with error number : -", errno);
		if(DEBUG) fflush(stderr);
		((struct com_thread_params*) arg)->data_thread_id = 0;
		((struct com_thread_params*) arg)->data_init_done = negative * errno;
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
	}
	((struct com_thread_params*) arg)->evfd = ret;  /* Save evfd for cleanup. */
	evfd = ((struct com_thread_params*) arg)->evfd;

	errno = 0;
	epfd = epoll_create(2);
	if(epfd < 0) {
		if(DEBUG) fprintf(stderr, "%s%d\n", "NATIVE data_looper() thread failed in epoll_create() with error number : -", errno);
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread exiting. Please RETRY registering data listener !");
		if(DEBUG) fflush(stderr);
		close(((struct com_thread_params*) arg)->evfd);
		((struct com_thread_params*) arg)->data_thread_id = 0;
		((struct com_thread_params*) arg)->data_init_done = negative * errno;
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
	}

	/* add serial port to epoll wait mechanism. Use level triggered epoll mechanism.  */
	ev_port.events = (EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP);
	ev_port.data.fd = fd;
	errno = 0;
	ret = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev_port);
	if(ret < 0) {
		if(DEBUG) fprintf(stderr, "%s%d\n", "NATIVE data_looper() thread failed in epoll_ctl() for adding serial port with error number : -", errno);
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread exiting. Please RETRY registering data listener !");
		if(DEBUG) fflush(stderr);
		close(epfd);
		close(((struct com_thread_params*) arg)->evfd);
		((struct com_thread_params*) arg)->data_thread_id = 0;
		((struct com_thread_params*) arg)->data_init_done = negative * errno;
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
	}

	/* add our thread exit signal fd to epoll wait mechanism. */
	ev_exit.events = (EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP);
	ev_exit.data.fd = ((struct com_thread_params*) arg)->evfd;
	errno = 0;
	ret = epoll_ctl(epfd, EPOLL_CTL_ADD, ((struct com_thread_params*) arg)->evfd, &ev_exit);
	if(ret < 0) {
		if(DEBUG) fprintf(stderr, "%s%d\n", "NATIVE data_looper() thread failed in epoll_ctl() for adding exit event evfd with error number : -", errno);
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread exiting. Please RETRY registering data listener !");
		if(DEBUG) fflush(stderr);
		close(epfd);
		close(((struct com_thread_params*) arg)->evfd);
		((struct com_thread_params*) arg)->data_thread_id = 0;
		((struct com_thread_params*) arg)->data_init_done = negative * errno;
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
	}
#endif
#if defined (__APPLE__)
	errno = 0;
	ret = pipe(pipe1);
	if(ret < 0) {
		if(DEBUG) fprintf(stderr, "%s%d\n", "NATIVE data_looper() thread failed in pipe() with error number : -", errno);
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread exiting. Please RETRY registering data listener !");
		if(DEBUG) fflush(stderr);
		((struct com_thread_params*) arg)->data_thread_id = 0;
		((struct com_thread_params*) arg)->data_init_done = negative * errno;
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
	}
	((struct com_thread_params*) arg)->evfd = pipe1[1];  /* Save writing end of pipe for exit and cleanup. */

	/* The kqueue() system call creates a new kernel event queue and returns a file descriptor. */
	errno = 0;
	kq = kqueue();
	if(kq < 0) {
		if(DEBUG) fprintf(stderr, "%s%d\n", "NATIVE data_looper() thread failed in kevent() for adding serial port with error number : -", errno);
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread exiting. Please RETRY registering data listener !");
		if(DEBUG) fflush(stderr);
		((struct com_thread_params*) arg)->data_thread_id = 0;
		((struct com_thread_params*) arg)->data_init_done = negative * errno;
		close(pipe1[0]);
		close(pipe1[1]);
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
	}

	/* Initialize what changes to be monitor on which fd. */
	EV_SET(&chlist[0], fd, EVFILT_READ, EV_ADD , 0, 0, NULL);
	EV_SET(&chlist[1], pipe1[0], EVFILT_READ, EV_ADD , 0, 0, NULL);
#endif

	/* indicate success to caller so it can return success to java layer */
	((struct com_thread_params*) arg)->data_init_done = 1;
	pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);

	/* This keep looping until listener is unregistered, waiting for data and passing it to java layer. */
	while(1) {

#if defined (__linux__)
	errno = 0;
	ret = epoll_wait(epfd, events, MAXEVENTS, -1);
	if(ret <= 0) {
		/* for error just restart looping. */
		continue;
	}
#endif
#if defined (__APPLE__)
	errno = 0;
	ret = kevent(kq, chlist, 2, evlist, 2, NULL);
	if(ret <= 0) {
		/* for error just restart looping. */
		continue;
	}

#endif

#if defined (__linux__)
	if((events[0].data.fd == evfd) || (events[1].data.fd == evfd)) {
		/* check if thread should exit due to un-registration of listener. */
		if(1 == ((struct com_thread_params*) arg)->data_thread_exit) {
			close(epfd);
			close(((struct com_thread_params*) arg)->evfd);
			((struct com_thread_params*) arg)->data_thread_id = 0;
			pthread_exit((void *)0);
		}
	}
#endif
#if defined (__APPLE__)
	/* Depending upon how many events has happened, pipe1[0] fd can be at 1st or 2nd
	 * index in evlist array. */
	if((evlist[0].ident == pipe1[0]) || (evlist[1].ident == pipe1[0])) {
		/* check if thread should exit due to un-registration of listener. */
		if(1 == ((struct com_thread_params*) arg)->data_thread_exit) {
			close(kq);
			close(pipe1[0]);
			close(pipe1[1]);
			((struct com_thread_params*) arg)->data_thread_id = 0;
			pthread_exit((void *)0);
		}
	}
#endif

#if defined (__linux__)
	if((events[0].events & EPOLLIN) && !(events[0].events & EPOLLERR)) {
#endif
#if defined (__APPLE__)
	if((evlist[0].ident == fd) && (evlist[0].flags & EVFILT_READ)) {
#endif
			/* input event happened, no error occurred, we have data to read on file descriptor. */
			do {
				errno = 0;
				ret = read(fd, buffer, sizeof(buffer));
				if(ret > 0 && errno == 0) {
					/* This indicates we got success and have read data. */
					/* If there is partial data read previously, append this data. */
					if(partialData == 1) {
						for(i = index; i < ret; i++) {
							final_buf[i] = buffer[i];
						}
						dataRead = (*env)->NewByteArray(env, index + ret);
						(*env)->SetByteArrayRegion(env, dataRead, 0, index + ret, final_buf);
						break;
					}else {
						/* Pass the successful read to java layer straight away. */
						dataRead = (*env)->NewByteArray(env, ret);
						(*env)->SetByteArrayRegion(env, dataRead, 0, ret, buffer);
						break;
					}
				}else if (ret > 0 && errno == EINTR) {
					/* This indicates, there is data to read, however, we got interrupted before we finish reading
					 * all of the available data. So we need to save this partial data and get back to read remaining. */
					for(i = index; i < ret; i++) {
						final_buf[i] = buffer[i];
					}
					index = ret;
					partialData = 1;
					continue;
				}else if(ret < 0) {
					if(errno == EAGAIN || errno == EWOULDBLOCK) {
						/* This indicates, there was no data to read. Therefore just return null. */
						dataRead = (*env)->NewByteArray(env, sizeof(empty_buf));
						(*env)->SetByteArrayRegion(env, dataRead, 0, sizeof(empty_buf), empty_buf);
						break;
					}else if(errno != EINTR) {
						/* This indicates, irrespective of, there was data to read or not, we got an error during operation. */
						/* Can we handle this condition more gracefully. */
						if(DEBUG) fprintf(stderr, "%s%d\n", "Native readBytes() failed to read data with error number : -", errno);
						if(DEBUG) fflush(stderr);

						dataRead = (*env)->NewByteArray(env, sizeof(empty_buf));
						(*env)->SetByteArrayRegion(env, dataRead, 0, sizeof(empty_buf), empty_buf);
						break;
					}else if (errno == EINTR) {
						/* This indicates that we should retry as we are just interrupted by a signal. */
						continue;
					}
				}else if (ret == 0) {
					/* This indicates, there was no data to read or EOF. */
					dataRead = (*env)->NewByteArray(env, sizeof(empty_buf));
					(*env)->SetByteArrayRegion(env, dataRead, 0, sizeof(empty_buf), empty_buf);
					break;
				}
			} while(1);

			/* once we have successfully read the data, let us pass this to java layer. */
			(*env)->CallVoidMethod(env, looper, mid, dataRead);
			if((*env)->ExceptionOccurred(env)) {
				LOGE(env);
			}
		}

	} /* Go back to loop again waiting for the data, available to read. */

	return ((void *)0);
}

/* This thread wait for a serial event to occur and enqueues it in event queue managed by java layer. */
/* TIOCMWAIT RETURNS -EIO IF DEVICE FROM USB PORT HAS BEEN REMOVED */
void *event_looper(void *arg) {
	int ret = 0;
	struct com_thread_params* params = (struct com_thread_params*) arg;
	JavaVM *jvm = (*params).jvm;
	int fd = (*params).fd;
	jobject looper = (*params).looper;

	int CTS =  0x01;  // 0000001
	int DSR =  0x02;  // 0000010
	int DCD =  0x04;  // 0000100
	int RI  =  0x08;  // 0001000
	int lines_status = 0;
	int cts,dsr,dcd,ri = 0;
	int event = 0;
	int oldstate = 0;
	int newstate = 0;

	pthread_mutex_lock(((struct com_thread_params*) arg)->mutex);

	/* The JNIEnv is valid only in the current thread. So, threads created should attach itself to the VM and obtain a JNI interface pointer. */
	void* env1;
	JNIEnv* env;
	if((*jvm)->AttachCurrentThread(jvm, &env1, NULL) != JNI_OK) {
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE event_looper() thread failed to attach itself to JVM.");
		if(DEBUG) fflush(stderr);
	}
	env = (JNIEnv*) env1;

	jclass SerialComLooper = (*env)->GetObjectClass(env, looper);
	if(SerialComLooper == NULL) {
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE event_looper() thread could not get class of object of type looper !");
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE event_looper() thread exiting. Please RETRY registering event listener !");
		if(DEBUG) fflush(stderr);
		((struct com_thread_params*) arg)->event_thread_id = 0;
		((struct com_thread_params*) arg)->event_init_done = -240;
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
	}

	jmethodID mid = (*env)->GetMethodID(env, SerialComLooper, "insertInEventQueue", "(I)V");
	if((*env)->ExceptionOccurred(env)) {
		LOGE(env);
	}
	if(mid == NULL) {
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread failed to retrieve method id of method insertInDataQueue in class SerialComLooper !");
		if(DEBUG) fprintf(stderr, "%s \n", "NATIVE data_looper() thread exiting. Please RETRY registering data listener !");
		if(DEBUG) fflush(stderr);
		((struct com_thread_params*) arg)->event_thread_id = 0;
		((struct com_thread_params*) arg)->event_init_done = -240;
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
	}

	/* Install signal handler that will be invoked to indicate that the thread should exit. */
    if(signal(SIGUSR1, exit_signal_handler) == SIG_ERR) {
    	if(DEBUG) fprintf(stderr, "%s\n", "Unable to create handler for SIGUSR1");
        if(DEBUG) fflush(stderr);
		((struct com_thread_params*) arg)->event_thread_id = 0;
		((struct com_thread_params*) arg)->event_init_done = -240;
		pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);
		pthread_exit((void *)0);
    }

	/* indicate success to caller so it can return success to java layer */
	((struct com_thread_params*) arg)->event_init_done = 1;
	pthread_mutex_unlock(((struct com_thread_params*) arg)->mutex);

	/* This keep looping until listener is unregistered, waiting for events and passing it to java layer.
	 * This sleep within the kernel until something happens to the MSR register of the tty device. */
	while(1) {
		lines_status = 0;
		cts = 0;
		dsr = 0;
		dcd = 0;
		ri = 0;
		event = 0;

#if defined (__linux__)
		/* When the user removes port on which this thread was calling this ioctl, this thread keep giving
		 * error -5 and keep looping in this ioctl for Linux. */
		errno = 0;
		ret = ioctl(fd, TIOCMIWAIT, TIOCM_CD | TIOCM_RNG | TIOCM_DSR | TIOCM_CTS);
		if(ret < 0) {
			if(DEBUG) fprintf(stderr, "%s%d\n", "NATIVE event_looper() failed in ioctl TIOCMIWAIT with error number : -", errno);
			if(DEBUG) fflush(stderr);
			continue;
		}
#endif
#if defined (__APPLE__)
		usleep(500000);
		if(DEBUG) fprintf(stderr, "%s%d\n", "ji",lines_status);
		if(DEBUG) fflush(stderr);

#endif

		/* Something happened on status line so get it. */
		errno = 0;
		ret = ioctl(fd, TIOCMGET, &lines_status);
		if(ret < 0) {
			if(DEBUG) fprintf(stderr, "%s%d\n", "NATIVE event_looper() failed in ioctl TIOCMGET with error number : -", errno);
			if(DEBUG) fflush(stderr);
			continue;
		}

		cts = (lines_status & TIOCM_CTS) ? 1 : 0;
		dsr = (lines_status & TIOCM_DSR) ? 1 : 0;
		dcd = (lines_status & TIOCM_CD)  ? 1 : 0;
		ri  = (lines_status & TIOCM_RI)  ? 1 : 0;

		if(cts) {
			event = event | CTS;
		}
		if(dsr) {
			event = event | DSR;
		}
		if(dcd) {
			event = event | DCD;
		}
		if(ri) {
			event = event | RI;
		}

#if defined (__linux__)
		if(DEBUG) fprintf(stderr, "%s %d\n", "NATIVE event_looper() sending bit mapped events ", event);
		if(DEBUG) fflush(stderr);
		/* Pass this to java layer inserting event in event queue. */
		(*env)->CallVoidMethod(env, looper, mid, event);
		if((*env)->ExceptionOccurred(env)) {
			LOGE(env);
		}
#endif
#if defined (__APPLE__)
		newstate = event;
		if(newstate != oldstate) {
			if(DEBUG) fprintf(stderr, "%s %d\n", "NATIVE event_looper() sending bit mapped events ", event);
			if(DEBUG) fflush(stderr);
			/* Pass this to java layer inserting event in event queue. */
			(*env)->CallVoidMethod(env, looper, mid, event);
			if((*env)->ExceptionOccurred(env)) {
				LOGE(env);
			}
			oldstate = newstate;
		}
#endif
	} /* Go back to loop again waiting for event to happen. */

	return ((void *)0);
}

#endif /* End compiling for Unix-like OS. */
