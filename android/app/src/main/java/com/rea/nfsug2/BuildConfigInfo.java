package com.rea.nfsug2;

import android.content.Context;
import android.content.pm.PackageManager;

final class BuildConfigInfo {
    private BuildConfigInfo() {}

    static String versionName(Context ctx) {
        try {
            return ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "?";
        }
    }
}
