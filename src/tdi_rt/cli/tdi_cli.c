/*
 * Copyright(c) 2021 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <Python.h>
#include <stdio.h>
#include <tdi/common/c_frontend/tdi_init.h>
#include <tdi/common/tdi_defs.h>
#include "target-utils/clish/shell.h"
#include <dvm/bf_drv_intf.h>
#include <target-sys/bf_sal/bf_sys_sem.h>
#include <lld/python_shell_mutex.h>
/*
 * tdi_python is not protected by bfshell's command-level lock.
 * We have a lock defined in python_shell_mutex to ensure multiple
 * instances of tdi_python aren't spawned. All python shell(tdi_python and
 * debug_python) share one lock.
 */

/*
 * Start TDI Runtime CLI. This program heavily borrows from the python C
 * interface example program.
 */
static int tdi_start_cli(int in_fd,
                         int out_fd,
                         const char *install_dir,
                         const char *udf,
                         bool interactive) {
  PyObject *pName = NULL, *pModule = NULL, *pFunc = NULL;
  PyObject *pArgs = NULL, *pValue = NULL;
  uint32_t array_size = 0;
  tdi_dev_id_t *dev_id_list = NULL;
  int ret_val = 0;

  Py_Initialize();

  tdi_num_device_id_list_get(&array_size);
  if (array_size) {
    dev_id_list = bf_sys_malloc(array_size * sizeof(tdi_dev_id_t));
    if (!dev_id_list) {
        fprintf(stderr, "Failed tp allocate device id list\n");
	ret_val = 1;
	goto cleanup;
    }
    tdi_device_id_list_get(dev_id_list);
  }

  /* Load the tdicli python program. Py_Initialize loads its libraries from
  the install dir we installed Python into. */
  pName = PyUnicode_DecodeFSDefault("tdicli");

  if (pName) {
      pModule = PyImport_Import(pName);
      Py_DECREF(pName);
  }

  if (pModule != NULL) {
    // Create a call to the start_tdi function in tdicli.py
    pFunc = PyObject_GetAttrString(pModule, "start_tdi");
    /* pFunc is a new reference */

    if (pFunc && PyCallable_Check(pFunc)) {
      // Use a tuple as our argument list
      if (udf) {
        pArgs = PyTuple_New(6);
      } else {
        pArgs = PyTuple_New(4);
      }

      if (!pArgs) {
          fprintf(stderr, "Failed to allocate pArgs\n");
	  ret_val = 1;
	  goto cleanup;
      }
      // Create python objects from c types.
      // Place references to them in the argument tuple.
      pValue = PyLong_FromLong(in_fd);
      PyTuple_SetItem(pArgs, 0, pValue);
      pValue = PyLong_FromLong(out_fd);
      PyTuple_SetItem(pArgs, 1, pValue);
      /*
       * Convert from the filepath c string to a python string using the
       * filesystem's default encoding
       */
      pValue = PyUnicode_DecodeFSDefault(install_dir);
      PyTuple_SetItem(pArgs, 2, pValue);
      PyObject *pyList = PyList_New(array_size);
      if (!pyList) {
	  fprintf(stderr, "Failed to allocate python list\n");
          ret_val = 1;
	  goto cleanup;
      }
      for (uint32_t i = 0; i < array_size; i++) {
        pValue = PyLong_FromLong(dev_id_list[i]);
        PyList_SetItem(pyList, i, pValue);
      }
      PyTuple_SetItem(pArgs, 3, pyList);
      if (udf) {
        pValue = PyUnicode_DecodeFSDefault(udf);
        PyTuple_SetItem(pArgs, 4, pValue);
        pValue = PyBool_FromLong(interactive);
        PyTuple_SetItem(pArgs, 5, pValue);
      }

      // Actually make the function call.
      // This is where we block until the CLI exits
      pValue = PyObject_CallObject(pFunc, pArgs);
      Py_DECREF(pArgs);

      // Handle exit codes and decrement references to free our python objects
      if (pValue != NULL) {
        long ret = PyLong_AsLong(pValue);
        if (ret == 0) {
          printf("tdi cli exited normally.\n");
        } else {
          printf("tdi cli exited with error: %ld\n", ret);
        }
        Py_DECREF(pValue);
      } else {
        Py_DECREF(pFunc);
        Py_DECREF(pModule);
        PyErr_Print();
        fprintf(stderr, "tdi cli python call failed\n");
        ret_val = 1;
        goto cleanup;
      }
    } else {
      if (PyErr_Occurred()) PyErr_Print();
      fprintf(stderr, "Cannot find start_tdi function.\n");
    }
    Py_XDECREF(pFunc);
    Py_DECREF(pModule);
  } else {
    PyErr_Print();
    fprintf(stderr, "Failed to load tdicli python library\n");
    ret_val = 1;
    goto cleanup;
  }
cleanup:
  // This will free all remaining memory allocated by python & the CLI.
  Py_Finalize();
  if (dev_id_list) {
    bf_sys_free(dev_id_list);
  }
  return ret_val;
}

// A klish plugin allowing BF Runtime CLI to be started from bfshell.
CLISH_PLUGIN_SYM(tdi_cli_cmd) {
  (void)script;
  (void)out;
  tdi_status_t sts;
  clish_shell_t *bfshell = clish_context__get_shell(clish_context);
  bool success = TRY_PYTHON_SHL_LOCK();
  if (!success) {
    bfshell_printf(clish_context,
                   "Only one Python shell instance allowed at a time. tdi "
                   "python and debug python share the python shell "
                   "resource.\n");
    return 0;
  }

  clish_pargv_t *pargv = clish_context__get_pargv(clish_context);
  const clish_parg_t *parg = clish_pargv_find_arg(pargv, "py_file");
  const char *udf = NULL;
  if (parg) {
    udf = clish_parg__get_value(parg);
  }
  parg = clish_pargv_find_arg(pargv, "interactive");
  const char *i_str = NULL;
  bool interactive = false;
  if (parg) {
    i_str = clish_parg__get_value(parg);
    if (i_str && strcmp(i_str, "1") == 0) {
      interactive = true;
    }
  }

  tinyrl_t *bftinyrl = clish_shell__get_tinyrl(bfshell);
  sts = tdi_start_cli(fileno(tinyrl__get_istream(bftinyrl)),
                        fileno(tinyrl__get_ostream(bftinyrl)),
                        clish_context__get_install_dir(clish_context),
                        udf,
                        interactive);
  if (sts != TDI_SUCCESS) {
    bfshell_printf(clish_context,
                   "%s:%d could not initialize tdi for the cli. err: %d\n",
                   __func__,
                   __LINE__,
                   sts);
  }
  RELEASE_PYTHON_SHL_LOCK();
  return 0;
}

CLISH_PLUGIN_SYM(tdi_run_file_cmd) {
  (void)script;
  (void)out;

  clish_shell_t *this = clish_context__get_shell(clish_context);
  clish_pargv_t *pargv = clish_context__get_pargv(clish_context);

  const char *filename = NULL;
  const clish_parg_t *parg = clish_pargv_find_arg(pargv, "filename");
  if (parg) {
    filename = clish_parg__get_value(parg);
  }

  int stop_on_error = 1;
  parg = clish_pargv_find_arg(pargv, "stop_on_error");
  if (parg) {
    const char *i_str = clish_parg__get_value(parg);
    if (i_str && strcmp(i_str, "0") == 0) {
      stop_on_error = 0;
    }
  }

  int result = -1;
  struct stat fileStat;

  /*
   * Check file specified is not a directory
   */
  if (filename && (0 == stat((char *)filename, &fileStat)) &&
      (!S_ISDIR(fileStat.st_mode))) {
    /*
     * push this file onto the file stack associated with this
     * session. This will be closed by clish_shell_pop_file()
     * when it is finished with.
     */
    result = clish_shell_push_file(this, filename, stop_on_error);
  }

  return result ? -1 : 0;
}

CLISH_PLUGIN_INIT(tdi) {
  (void)clish_shell;
  clish_plugin_add_sym(plugin, tdi_cli_cmd, "tdi_cli_cmd");
  clish_plugin_add_sym(plugin, tdi_run_file_cmd, "tdi_run_file_cmd");
  return 0;
}
