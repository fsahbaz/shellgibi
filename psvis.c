#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/time.h>

static int PID = -50;
int **tree_pid, **tree_time;

module_param(PID, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(myint, "Entered PID: \n");

void DFS(struct task_struct *task, int **tree_pid, int **tree_time, int depth)
{   
    struct task_struct *child;
    struct list_head *list;
    int i = 0;
    int j = 0;
    for(; i<depth; i++) {
        printk(KERN_CONT "-");
    }
    
    printk(KERN_CONT "PID: %d, Creation Time: %d\n", task->pid, task->start_time);
    list_for_each(list, &task->children) {
        child = list_entry(list, struct task_struct, sibling);
        tree_pid[depth+1][j] = (int) child->pid;
        tree_time[depth+1][j] = (int) child->start_time;
        DFS(child, tree_pid, tree_time, depth+1);
        j++;
    }
}


/* This function is called when the module is loaded. */
int proc_init(void)
{
    printk(KERN_INFO "Loading PSVIS Module\n");
    // checking the given PID
    if (PID < 0)
    {
        printk(KERN_INFO "Wrong PID, going to unload.\n");
        return 1;
    }
    else // valid PID
    {
        struct task_struct *task;
        tree_pid = kmalloc(100 * sizeof(int*), GFP_KERNEL);
        tree_time = kmalloc(100 * sizeof(int*), GFP_KERNEL);
        int i;
        for (i = 0; i < 100; i++) {
            tree_pid[i] = kmalloc(100 * sizeof(int), GFP_KERNEL);
            tree_time[i] = kmalloc(100 * sizeof(int), GFP_KERNEL);
        }
        // finding the task with given PID
        task = pid_task(find_vpid((pid_t)PID), PIDTYPE_PID);
        if (task != NULL) {
            tree_pid[0][0] = (int) task->pid;
            tree_time[0][0] = (int) task->start_time;
            DFS(task,tree_pid,tree_time,0);
        } else {
            printk(KERN_ALERT "Process with PID %d is not found.",PID);
        }
        
    }

    return 0;
}

/* This function is called when the module is removed. */
void proc_exit(void)
{
    int i;
    for (i = 0; i < 100; i++) {
        kfree(tree_pid[i]);
        kfree(tree_time[i]);
    }
    kfree(tree_pid);
    kfree(tree_time);
    printk(KERN_INFO "Removing PSVIS Module\n");
}
/* Macros for registering module entry and exit points. */
module_init(proc_init);
module_exit(proc_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PSVIS Module");
MODULE_AUTHOR("Ahmet Uysal & Furkan Sahbaz");