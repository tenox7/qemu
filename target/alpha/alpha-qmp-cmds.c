/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU Alpha CPU (monitor definitions)
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qobject-input-visitor.h"
#include "qobject/qdict.h"
#include "qom/qom-qobject.h"
#include "system/qmp-cpu.h"
#include "cpu.h"

static void alpha_cpu_defs_entry(gpointer data, gpointer user_data)
{
    ObjectClass *oc = data;
    CpuDefinitionInfoList **first = user_data;
    const char *typename;
    CpuDefinitionInfo *info;

    typename = object_class_get_name(oc);
    info = g_malloc0(sizeof(*info));
    info->name = cpu_model_from_type(typename);
    info->q_typename = g_strdup(typename);

    QAPI_LIST_PREPEND(*first, info);
}

CpuDefinitionInfoList *qmp_query_alpha_cpu_definitions(Error **errp)
{
    CpuDefinitionInfoList *cpu_list = NULL;
    GSList *list;
    int i;

    list = object_class_get_list(TYPE_ALPHA_CPU, false);
    g_slist_foreach(list, alpha_cpu_defs_entry, &cpu_list);
    g_slist_free(list);

    for (i = 0; alpha_cpu_aliases[i].alias != NULL; i++) {
        const AlphaCPUAlias *alias = &alpha_cpu_aliases[i];
        ObjectClass *oc;
        CpuDefinitionInfo *info;

        oc = alpha_cpu_class_by_name(alias->model);
        if (oc == NULL) {
            continue;
        }

        info = g_malloc0(sizeof(*info));
        info->name = g_strdup(alias->alias);
        info->q_typename = g_strdup(object_class_get_name(oc));

        QAPI_LIST_PREPEND(cpu_list, info);
    }

    return cpu_list;
}
