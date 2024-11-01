/*
 * virnwfilterbindingobj.c: network filter binding object processing
 *
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <unistd.h>

#include "viralloc.h"
#include "virerror.h"
#include "virstring.h"
#include "nwfilter_params.h"
#include "virnwfilterbindingobj.h"
#include "viruuid.h"
#include "virfile.h"


#define VIR_FROM_THIS VIR_FROM_NWFILTER

struct _virNWFilterBindingObj {
    virObjectLockable parent;

    bool removing;
    virNWFilterBindingDef *def;
};


static virClass *virNWFilterBindingObjClass;
static void virNWFilterBindingObjDispose(void *obj);

static int
virNWFilterBindingObjOnceInit(void)
{
    if (!VIR_CLASS_NEW(virNWFilterBindingObj, virClassForObjectLockable()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virNWFilterBindingObj);

virNWFilterBindingObj *
virNWFilterBindingObjNew(void)
{
    if (virNWFilterBindingObjInitialize() < 0)
        return NULL;

    return virObjectNew(virNWFilterBindingObjClass);
}


static void
virNWFilterBindingObjDispose(void *obj)
{
    virNWFilterBindingObj *bobj = obj;

    virNWFilterBindingDefFree(bobj->def);
}


virNWFilterBindingDef *
virNWFilterBindingObjGetDef(virNWFilterBindingObj *obj)
{
    return obj->def;
}


void
virNWFilterBindingObjSetDef(virNWFilterBindingObj *obj,
                            virNWFilterBindingDef *def)
{
    virNWFilterBindingDefFree(obj->def);
    obj->def = def;
}


virNWFilterBindingDef *
virNWFilterBindingObjStealDef(virNWFilterBindingObj *obj)
{
    return g_steal_pointer(&obj->def);
}


bool
virNWFilterBindingObjGetRemoving(virNWFilterBindingObj *obj)
{
    return obj->removing;
}


void
virNWFilterBindingObjSetRemoving(virNWFilterBindingObj *obj,
                                 bool removing)
{
    obj->removing = removing;
}


/**
 * virNWFilterBindingObjEndAPI:
 * @obj: binding object
 *
 * Finish working with a binding object in an API.  This function
 * clears whatever was left of a domain that was gathered using
 * virNWFilterBindingObjListFindByPortDev(). Currently that means
 * only unlocking and decrementing the reference counter of that
 * object. And in order to make sure the caller does not access
 * the object, the pointer is cleared.
 */
void
virNWFilterBindingObjEndAPI(virNWFilterBindingObj **obj)
{
    if (!*obj)
        return;

    virObjectUnlock(*obj);
    virObjectUnref(*obj);
    *obj = NULL;
}


char *
virNWFilterBindingObjConfigFile(const char *dir,
                                const char *name)
{
    char *ret;

    ret = g_strdup_printf("%s/%s.xml", dir, name);
    return ret;
}


int
virNWFilterBindingObjSave(const virNWFilterBindingObj *obj,
                          const char *statusDir)
{
    char *filename;
    char *xml = NULL;
    int ret = -1;

    if (!(filename = virNWFilterBindingObjConfigFile(statusDir,
                                                     obj->def->portdevname)))
        return -1;

    if (!(xml = virNWFilterBindingObjFormat(obj)))
        goto cleanup;

    if (g_mkdir_with_parents(statusDir, 0777) < 0) {
        virReportSystemError(errno,
                             _("cannot create config directory '%s'"),
                             statusDir);
        goto cleanup;
    }

    ret = virXMLSaveFile(filename,
                         obj->def->portdevname, "nwfilter-binding-create",
                         xml);

 cleanup:
    VIR_FREE(xml);
    VIR_FREE(filename);
    return ret;
}


int
virNWFilterBindingObjDelete(const virNWFilterBindingObj *obj,
                            const char *statusDir)
{
    char *filename;
    int ret = -1;

    if (!(filename = virNWFilterBindingObjConfigFile(statusDir,
                                                     obj->def->portdevname)))
        return -1;

    if (unlink(filename) < 0 &&
        errno != ENOENT) {
        virReportSystemError(errno,
                             _("Unable to remove status '%s' for nwfilter binding %s'"),
                             filename, obj->def->portdevname);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    VIR_FREE(filename);
    return ret;
}


static virNWFilterBindingObj *
virNWFilterBindingObjParseXML(xmlDocPtr doc,
                              xmlXPathContextPtr ctxt)
{
    virNWFilterBindingObj *ret;
    xmlNodePtr node;

    if (!(ret = virNWFilterBindingObjNew()))
        return NULL;

    if (!(node = virXPathNode("./filterbinding", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("filter binding status missing content"));
        goto cleanup;
    }

    if (!(ret->def = virNWFilterBindingDefParseNode(doc, node)))
        goto cleanup;

    return ret;

 cleanup:
    virObjectUnref(ret);
    return NULL;
}


static virNWFilterBindingObj *
virNWFilterBindingObjParseNode(xmlDocPtr doc,
                               xmlNodePtr root)
{
    g_autoptr(xmlXPathContext) ctxt = NULL;

    if (STRNEQ((const char *)root->name, "filterbindingstatus")) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("unknown root element '%s' for filter binding"),
                       root->name);
        return NULL;
    }

    if (!(ctxt = virXMLXPathContextNew(doc)))
        return NULL;

    ctxt->node = root;
    return virNWFilterBindingObjParseXML(doc, ctxt);
}


static virNWFilterBindingObj *
virNWFilterBindingObjParse(const char *xmlStr,
                           const char *filename)
{
    virNWFilterBindingObj *obj = NULL;
    g_autoptr(xmlDoc) xml = NULL;

    if ((xml = virXMLParse(filename, xmlStr, _("(nwfilterbinding_status)"), NULL, false))) {
        obj = virNWFilterBindingObjParseNode(xml, xmlDocGetRootElement(xml));
    }

    return obj;
}


virNWFilterBindingObj *
virNWFilterBindingObjParseFile(const char *filename)
{
    return virNWFilterBindingObjParse(NULL, filename);
}


char *
virNWFilterBindingObjFormat(const virNWFilterBindingObj *obj)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "<filterbindingstatus>\n");

    virBufferAdjustIndent(&buf, 2);

    if (virNWFilterBindingDefFormatBuf(&buf, obj->def) < 0)
        return NULL;

    virBufferAdjustIndent(&buf, -2);
    virBufferAddLit(&buf, "</filterbindingstatus>\n");

    return virBufferContentAndReset(&buf);
}
