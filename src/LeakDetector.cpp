#include "LeakDetector.h"

#undef malloc
#undef calloc
#undef free

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "Platform.h"

#ifdef _MSC_VER
#       pragma warning (disable: 4996) // 'argument': This function or variable may be unsafe.
#endif

namespace LeakDetector {

    using namespace std;

    namespace {

        // Node of Memory Leak Info
        struct LEAK_INFO
        {
            // Memory Allocation Info
            struct MEM_INFO
            {
                void    *address;
                size_t  size;
                char    filename[FN_SIZE];
                u32     line_no;

            } mem_info;

            LEAK_INFO *next;

        };


        LEAK_INFO *pHead = NULL;
        LEAK_INFO *pCurr = NULL;

        // Makes and appends the allocated memory info to the list
        void append_mem_info (void *mem_ref, size_t size, const char filename[], u32 line_no)
        {
            // append the above info to the list
            LEAK_INFO *p_new = (LEAK_INFO *) malloc (sizeof (LEAK_INFO));
            if (p_new)
            {
                p_new->mem_info.address   = mem_ref;
                p_new->mem_info.size      = size;
                strncpy_s (p_new->mem_info.filename, FN_SIZE, filename, FN_SIZE);
                p_new->mem_info.line_no   = line_no;
                p_new->next = NULL;

                if (pCurr)
                {
                    pCurr->next = p_new;
                    pCurr       = pCurr->next;
                }
                else
                {
                    pCurr = pHead = p_new;
                }
            }
        }
        // Removes the allocated memory info if is part of the list
        void remove_mem_info (void *mem_ref)
        {
            LEAK_INFO *p_old = NULL;
            LEAK_INFO *p_itr = pHead;
            // check if allocate memory is in list
            while (p_itr)
            {
                if (p_itr->mem_info.address == mem_ref)
                {
                    if (p_old)
                    {
                        p_old->next = p_itr->next;
                        free (p_itr);
                    }
                    else
                    {
                        LEAK_INFO *p_tmp = pHead;
                        pHead = pHead->next;
                        free (p_tmp);
                    }

                    return;
                }

                p_old = p_itr;
                p_itr = p_itr->next;
            }
        }

        // Clears all the allocated memory info from the list
        void clear_mem_info ()
        {
            pCurr = pHead;
            while (pCurr)
            {
                LEAK_INFO *p_tmp = pCurr;
                pCurr = pCurr->next;
                free (p_tmp);
            }
        }

    }

    // Replacement of malloc
    void* xmalloc (u64 mem_size, const char filename[], u32 line_no)
    {
        void *mem_ref = malloc (mem_size);
        if (mem_ref)
        {
            append_mem_info (mem_ref, mem_size, filename, line_no);
        }
        return mem_ref;
    }
    // Replacement of calloc
    void* xcalloc (u64 count, u64 mem_size, const char filename[], u32 line_no)
    {
        void *mem_ref = calloc (count, mem_size);
        if (mem_ref)
        {
            append_mem_info (mem_ref, count * mem_size, filename, line_no);
        }
        return mem_ref;
    }
    // Replacement of free
    void  xfree (void *mem_ref)
    {
        remove_mem_info (mem_ref);
        free (mem_ref);
    }

    // Writes all info of the unallocated memory into a output file
    void report_memleakage ()
    {
        FILE *fp_write = fopen (INFO_FN, "wb");
        //errno_t err = fopen_s (&fp_write, INFO_FN, "wb");

        if (fp_write)
        {
            char info_buf[1024];
            LEAK_INFO *leak_info;
            leak_info = pHead;

            i32 x;
            x = sprintf (info_buf, "%s\n", "Memory Leak Summary");
            //x = sprintf_s (info_buf, BUF_SIZE, "%s\n", "Memory Leak Summary");
            fwrite (info_buf, strlen (info_buf) + 1, 1, fp_write);
            x = sprintf (info_buf, "%s\n", "-----------------------------------");
            //x = sprintf_s (info_buf, BUF_SIZE, "%s\n", "-----------------------------------");
            fwrite (info_buf, strlen (info_buf) + 1, 1, fp_write);

            while (leak_info)
            {
                x = sprintf (info_buf, "Address : %p\n", leak_info->mem_info.address);
                //x = sprintf_s (info_buf, BUF_SIZE, "Address : %p\n", leak_info->mem_info.address);
                fwrite (info_buf, strlen (info_buf) + 1, 1, fp_write);
                x = sprintf (info_buf, "Size    : %u bytes\n", leak_info->mem_info.size);
                //x = sprintf_s (info_buf, BUF_SIZE, "Size    : %lu bytes\n", leak_info->mem_info.size);
                fwrite (info_buf, strlen (info_buf) + 1, 1, fp_write);
                x = sprintf (info_buf, "Filename: %s\n", leak_info->mem_info.filename);
                //x = sprintf_s (info_buf, BUF_SIZE, "Filename: %s\n", leak_info->mem_info.filename);
                fwrite (info_buf, strlen (info_buf) + 1, 1, fp_write);
                x = sprintf (info_buf, "Line #  : %u\n", leak_info->mem_info.line_no);
                //x = sprintf_s (info_buf, BUF_SIZE, "Line #  : %u\n", leak_info->mem_info.line_no);
                fwrite (info_buf, strlen (info_buf) + 1, 1, fp_write);
                x = sprintf (info_buf, "%s\n", "-----------------------------------");
                //x = sprintf_s (info_buf, BUF_SIZE, "%s\n", "-----------------------------------");
                fwrite (info_buf, strlen (info_buf) + 1, 1, fp_write);

                leak_info = leak_info->next;
            }
            fclose (fp_write);
        }

        clear_mem_info ();
    }

}
