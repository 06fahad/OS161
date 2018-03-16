#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <machine/pcb.h>
#include <machine/spl.h>
#include <machine/trapframe.h>
#include <kern/callno.h>
#include <syscall.h>
#include <uio.h>
#include <elf.h>
#include <addrspace.h>
#include <thread.h>
#include <curthread.h>
#include <vnode.h>
#include <vfs.h>
#include <kern/unistd.h>



/*
 * System call handler.
 *
 * A pointer to the trapframe created during exception entry (in
 * exception.S) is passed in.
 *
 * The calling conventions for syscalls are as follows: Like ordinary
 * function calls, the first 4 32-bit arguments are passed in the 4
 * argument registers a0-a3. In addition, the system call number is
 * passed in the v0 register.
 *
 * On successful return, the return value is passed back in the v0
 * register, like an ordinary function call, and the a3 register is
 * also set to 0 to indicate success.
 *
 * On an error return, the error code is passed back in the v0
 * register, and the a3 register is set to 1 to indicate failure.
 * (Userlevel code takes care of storing the error code in errno and
 * returning the value -1 from the actual userlevel syscall function.
 * See src/lib/libc/syscalls.S and related files.)
 *
 * Upon syscall return the program counter stored in the trapframe
 * must be incremented by one instruction; otherwise the exception
 * return code will restart the "syscall" instruction and the system
 * call will repeat forever.
 *
 * Since none of the OS/161 system calls have more than 4 arguments,
 * there should be no need to fetch additional arguments from the
 * user-level stack.
 *
 * Watch out: if you make system calls that have 64-bit quantities as
 * arguments, they will get passed in pairs of registers, and not
 * necessarily in the way you expect. We recommend you don't do it.
 * (In fact, we recommend you don't use 64-bit quantities at all. See
 * arch/mips/include/types.h.)
 */

void
mips_syscall(struct trapframe *tf)
{
	int callno;
	int32_t retval;
	int err;

	assert(curspl==0);

	callno = tf->tf_v0;

	/*
	 * Initialize retval to 0. Many of the system calls don't
	 * really return a value, just 0 for success and -1 on
	 * error. Since retval is the value returned on success,
	 * initialize it to 0 by default; thus it's not necessary to
	 * deal with it except for calls that return other values, 
	 * like write.
	 */

	retval = 0;

	switch (callno) {
		case SYS__exit:
	    thread_exit();
	    break;
	    
	    case SYS_reboot:
		err = sys_reboot(tf->tf_a0);
		break;

	    case SYS_write:
	    err = sys_write(tf->tf_a0, tf->tf_a1, tf->tf_a2, &retval);
	    break;
 		
		case SYS_read:
		err = sys_read(tf->tf_a0, tf->tf_a1, tf->tf_a2, &retval);
		break;

		case SYS_fork:
		err = sys_fork(tf, &retval, sys_getpid(), &retval);
		break;

		case SYS_getpid:
		err = sys_getpid();
		break;

		case SYS_execv:
		err = sys_execv(tf);
		break;

		default:
		kprintf("Unknown syscall %d\n", callno);
		err = ENOSYS;
		break;
	}


	if (err) {
		/*
		 * Return the error code. This gets converted at
		 * userlevel to a return value of -1 and the error
		 * code in errno.
		 */
		tf->tf_v0 = err;
		tf->tf_a3 = 1;      /* signal an error */
	}
	else {
		/* Success. */
		tf->tf_v0 = retval;
		tf->tf_a3 = 0;      /* signal no error */
	}
	
	/*
	 * Now, advance the program counter, to avoid restarting
	 * the syscall over and over again.
	 */
	
	tf->tf_epc += 4;

	/* Make sure the syscall code didn't forget to lower spl */
	assert(curspl==0);
}


int sys_execv(struct trapframe *tf) {
	
	char *prog = (char *) tf->tf_a0;
	char **args = (char **) tf->tf_a1;

	int argc = 0; //to count number of arguments
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;
	struct addrspace *old_addrspace;

	/* Open the file. */
	char **arg_array = args;
	int i = 0;
	while (args[i] != NULL) {
		argc++;
		i++;
	}
	//kprintf("args\n");
	char *program = (char *)kmalloc(strlen(args[0]) + 1);
	copyinstr(prog, program, strlen(args[0])+1, NULL);

	//kprintf("program: %s\n", program);

	char* argv[argc];
	int j;
	for(j = 0; j < argc; j++) {
		argv[j] = (char *)kmalloc(strlen(args[j]) + 1);
		copyinstr(args[j], argv[j], strlen(args[j])+1, NULL);
	}

	//kprintf("argv[1]: %s\n", argv[1]);
	
	
	int ret = runprogram(program, argv, argc);
	return ret;
}

int sys_getpid() {
	return curthread->pid;
}

void
md_forkentry_mod(struct trapframe *tf, struct addrspace *as)
{
	/*
	 * This function is provided as a reminder. You need to write
	 * both it and the code that calls it.
	 *
	 * Thus, you can trash it and do things another way if you prefer.
	 */
	curthread->t_vmspace = as;
	//struct trapframe temp_tf;
	//memcpy(&temp_tf, tf, sizeof(struct trapframe));
	struct trapframe temp_tf = *tf;
	//struct addrspace* cur_asspace;
	//cur_asspace = as;
	//temp_tf = *cpy_tf;
	temp_tf.tf_v0 = 0;
	temp_tf.tf_a3 = 0;
	//kprintf("pc before: %x\n", temp_tf.tf_epc);
	temp_tf.tf_epc += 4;
	//kprintf("pc after: %x\n", temp_tf.tf_epc);

	//curthread->t_vmspace = cur_asspace;
	

	as_activate(curthread->t_vmspace);

	kfree(tf);
	mips_usermode(&temp_tf);

}

int sys_fork(struct trapframe *tf, int32_t *return_val, pid_t parent_pid, int32_t *retval) {
	kprintf("pc before: %x\n", tf->tf_epc);
	struct thread *child_thread;
	struct trapframe *tf_copy;
	tf_copy = (struct trapframe*) kmalloc(sizeof(struct trapframe));
	memcpy(tf_copy, tf, sizeof(struct trapframe));
	struct addrspace *temp_addrspace;
	int copy_ret = as_copy(curthread->t_vmspace, &temp_addrspace);
	
	if(copy_ret) {
		//take care of errors here
	}

	int thread_fork_ret = thread_fork_mod("child process", tf_copy, temp_addrspace, md_forkentry_mod, &child_thread);

	kprintf("We are returning child process wih pid :%d\n", child_thread->pid);
	*retval = child_thread->pid;
	return 0;

}


int sys_read(int fd, const void *buf, size_t nbytes, int32_t* return_val) {
	int result;

	if (fd == 1 || fd == 2) {
		if(buf == NULL) {
			return EFAULT;
		} 
		//else {
			struct vnode *vn;
			struct uio u;
			char *console = NULL;
			console = kstrdup("con:");
			result = vfs_open(console, 0, &vn);
			//kprintf("vfs_open result: %d\n", result);
			//kfree(console);

			/*u.uio_iovec.iov_ubase = (userptr_t)vaddr;
			u.uio_iovec.iov_len = memsize;   // length of the memory space
			u.uio_resid = filesize;          // amount to actually read
			u.uio_offset = offset;
			u.uio_segflg = is_executable ? UIO_USERISPACE : UIO_USERSPACE;
			u.uio_rw = UIO_READ;
			u.uio_space = curthread->t_vmspace;
			*/
			u.uio_iovec.iov_ubase = (userptr_t)buf;
			u.uio_iovec.iov_len = nbytes;
			u.uio_resid = nbytes;
			u.uio_rw = UIO_READ;
			u.uio_offset = 0;
			u.uio_segflg = UIO_USERSPACE;
			u.uio_space = curthread->t_vmspace;

			result = VOP_WRITE(vn, &u);
			//kprintf("COP_Write result: %d\n", result);
			*return_val = nbytes;
			if(result) {
				return result;

			}
			vfs_close(vn);
			return 0;
		//}
		
	} else {
		return EBADF;
	}
}





//write system call function impelementation
int sys_write (int fd, const void *buf, size_t nbytes, int32_t* return_val) {

	int result;

	if (fd == 1 || fd == 2) {
		if(buf == NULL) {
			return EFAULT;
		} 
		//else {
			struct vnode *vn;
			struct uio u;
			char *console = NULL;
			console = kstrdup("con:");
			result = vfs_open(console, 1, &vn);
			if(result) {
				return result;

			}
			//kprintf("vfs_open result: %d\n", result);
			//kfree(console);

			/*u.uio_iovec.iov_ubase = (userptr_t)vaddr;
			u.uio_iovec.iov_len = memsize;   // length of the memory space
			u.uio_resid = filesize;          // amount to actually read
			u.uio_offset = offset;
			u.uio_segflg = is_executable ? UIO_USERISPACE : UIO_USERSPACE;
			u.uio_rw = UIO_READ;
			u.uio_space = curthread->t_vmspace;
			*/
			u.uio_iovec.iov_ubase = (userptr_t)buf;
			u.uio_iovec.iov_len = nbytes;
			u.uio_resid = nbytes;
			u.uio_rw = UIO_WRITE;
			u.uio_offset = 0;
			u.uio_segflg = UIO_USERSPACE;
			u.uio_space = curthread->t_vmspace;

			result = VOP_WRITE(vn, &u);
			//kprintf("COP_Write result: %d\n", result);
			*return_val = nbytes;
			if(result) {
				return result;

			}
			vfs_close(vn);
			return 0;
		//}
		
	} else {
		return EBADF;
	}
}