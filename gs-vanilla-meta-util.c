/*
 * Copyright (C) 2023 Mateus Melchiades
 */

#include "gs-vanilla-meta-util.h"

void
gs_vanilla_meta_app_set_packaging_info(GsApp *app)
{
    g_return_if_fail(GS_IS_APP(app));

    /* gs_app_set_bundle_kind(app, AS_BUNDLE_KIND_PACKAGE); */
    gs_app_set_metadata(app, "GnomeSoftware::PackagingFormat", "Apx");
    /* gs_app_set_metadata(app, "GnomeSoftware::SortKey", "200"); */
    gs_app_set_metadata(app, "GnomeSoftware::PackagingBaseCssColor", "warning_color");
    gs_app_set_metadata(app, "GnomeSoftware::PackagingIcon", "org.vanillaos.FirstSetup-symbolic");
}
