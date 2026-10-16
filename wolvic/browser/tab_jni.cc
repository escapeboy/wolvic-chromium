// Copyright 2023 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wolvic/jni_headers/Tab_jni.h"

#include "base/android/callback_android.h"
#include "base/android/jni_array.h"
#include "base/android/jni_string.h"
#include "base/json/json_writer.h"
#include "components/webapps/browser/installable/installable_manager.h"
#include "components/zoom/page_zoom.h"
#include "components/zoom/zoom_controller.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/cors_origin_pattern_setter.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/page_zoom.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "services/network/public/mojom/cors_origin_pattern.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "wolvic/browser/autocomplete/wolvic_autofill_client.h"
#include "wolvic/browser/autocomplete/wolvic_password_manager_client.h"
#include "wolvic/browser/wolvic_contents.h"
#include "wolvic/browser/wolvic_web_contents_delegate.h"
#include "wolvic/wolvic_browser_context.h"
#include "wolvic/wolvic_content_browser_client.h"

using base::android::JavaRef;
using base::android::ScopedJavaLocalRef;
using content::WebContents;

namespace wolvic {

namespace {

void doPageZoom(const JavaRef<jobject>& jweb_contents,
                content::PageZoom zoom) {
  WebContents* web_contents = WebContents::FromJavaWebContents(jweb_contents);
  DCHECK(web_contents);
  zoom::PageZoom::Zoom(web_contents, zoom);
}

}  // namespace

void JNI_Tab_AttachWebContents(JNIEnv* env,
                            const JavaRef<jobject>& jweb_contents) {
  WebContents* web_contents = WebContents::FromJavaWebContents(jweb_contents);
  DCHECK(web_contents);
  auto wolvic_contents = std::make_unique<WolvicContents>(
      std::unique_ptr<WebContents>(web_contents));
  wolvic_contents.release()->Init();
  // WebContents reaching this path were created by the web engine itself (for
  // example target=_blank navigations) rather than by WolvicWebContentsFactory,
  // so they still need a ZoomController of their own.
  zoom::ZoomController::CreateForWebContents(web_contents);
  web_contents->ResumeLoadingCreatedWebContents();
}

void JNI_Tab_ReleaseWebContents(JNIEnv* env,
                                const JavaRef<jobject>& jweb_contents) {
  WebContents* web_contents = WebContents::FromJavaWebContents(jweb_contents);
  DCHECK(web_contents);

  auto* wolvic_contents = WolvicContents::FromWebContents(web_contents);
  DCHECK(wolvic_contents);
  delete wolvic_contents;
}

void JNI_Tab_SetWebContentsDelegate(
    JNIEnv* env,
    const JavaRef<jobject>& jweb_contents,
    const JavaRef<jobject>& jweb_contents_delegate) {
  WebContents* web_contents = WebContents::FromJavaWebContents(jweb_contents);
  DCHECK(web_contents);

  auto* wolvic_contents = WolvicContents::FromWebContents(web_contents);
  auto web_contents_delegate =
      std::make_unique<WolvicWebContentsDelegate>(env, jweb_contents_delegate);
  wolvic_contents->SetDelegate(std::move(web_contents_delegate));

  webapps::InstallableManager::CreateForWebContents(web_contents);
  WolvicAutofillClient::CreateForWebContents(web_contents);
  WolvicPasswordManagerClient::CreateForWebContents(web_contents);
}

void JNI_Tab_PageZoomIn(JNIEnv* env,
                        const JavaRef<jobject>& jweb_contents) {
  doPageZoom(jweb_contents, content::PAGE_ZOOM_IN);
}

void JNI_Tab_PageZoomOut(JNIEnv* env,
                         const JavaRef<jobject>& jweb_contents) {
  doPageZoom(jweb_contents, content::PAGE_ZOOM_OUT);
}

void JNI_Tab_PageZoomReset(JNIEnv* env,
                           const JavaRef<jobject>& jweb_contents) {
  doPageZoom(jweb_contents, content::PAGE_ZOOM_RESET);
}

jint JNI_Tab_GetCurrentZoomLevel(JNIEnv* env,
                                 const JavaRef<jobject>& jweb_contents) {
  WebContents* web_contents = WebContents::FromJavaWebContents(jweb_contents);
  DCHECK(web_contents);

  zoom::ZoomController* zoom_controller =
      zoom::ZoomController::FromWebContents(web_contents);
  if (zoom_controller)
    return zoom_controller->GetZoomPercent();

  double zoom_factor = blink::ZoomLevelToZoomFactor(
      zoom::ZoomController::GetZoomLevelForWebContents(web_contents));
  return static_cast<int>(zoom_factor * 100 + 0.5);
}

// Stena: runs `script` in the primary main frame and hands its completion
// value to `jcallback` as JSON ("null" when there is none or it cannot be
// serialized). world_id 0 is the page's own world, which Chromium only allows
// after AllowInjectingJavaScript(); any other id is an isolated world (the model
// of an extension content script), offset past content's own ids. Promises are
// not awaited.
void JNI_Tab_ExecuteScript(JNIEnv* env,
                           const JavaRef<jobject>& jweb_contents,
                           const JavaRef<jstring>& jscript,
                           jint world_id,
                           const JavaRef<jobject>& jcallback) {
  WebContents* web_contents = WebContents::FromJavaWebContents(jweb_contents);
  DCHECK(web_contents);
  std::u16string script = base::android::ConvertJavaStringToUTF16(env, jscript);
  auto done = base::BindOnce(
      [](const base::android::ScopedJavaGlobalRef<jobject>& callback,
         base::Value result) {
        base::android::RunStringCallbackAndroid(
            callback, base::WriteJson(result).value_or("null"));
      },
      base::android::ScopedJavaGlobalRef<jobject>(jcallback));
  content::RenderFrameHost* frame = web_contents->GetPrimaryMainFrame();
  if (world_id == 0) {
    content::RenderFrameHost::AllowInjectingJavaScript();
    frame->ExecuteJavaScript(script, std::move(done));
    return;
  }
  CHECK_GT(world_id, 0);
  frame->ExecuteJavaScriptInIsolatedWorld(
      script, std::move(done),
      content::ISOLATED_WORLD_ID_CONTENT_END + world_id - 1);
}

// Stena: lets pages of `jorigin` read https resources of `jhosts` without CORS
// headers, in the tab's browser context. This is the mechanism behind an
// extension's host permissions (extensions/browser/extension_util.cc). Each call
// replaces the list for that origin. `jdone` gets true once every storage
// partition has it.
void JNI_Tab_AllowCrossOriginReads(JNIEnv* env,
                                   const JavaRef<jobject>& jweb_contents,
                                   const JavaRef<jstring>& jorigin,
                                   const JavaRef<jobjectArray>& jhosts,
                                   const JavaRef<jobject>& jdone) {
  WebContents* web_contents = WebContents::FromJavaWebContents(jweb_contents);
  DCHECK(web_contents);
  url::Origin origin = url::Origin::Create(
      GURL(base::android::ConvertJavaStringToUTF8(env, jorigin)));
  std::vector<std::string> hosts;
  base::android::AppendJavaStringArrayToStringVector(env, jhosts, &hosts);
  std::vector<network::mojom::CorsOriginPatternPtr> allow;
  for (const std::string& host : hosts) {
    allow.push_back(network::mojom::CorsOriginPattern::New(
        "https", host, /*port=*/0,
        network::mojom::CorsDomainMatchMode::kDisallowSubdomains,
        network::mojom::CorsPortMatchMode::kAllowAnyPort,
        network::mojom::CorsOriginAccessMatchPriority::kDefaultPriority));
  }
  content::CorsOriginPatternSetter::Set(
      web_contents->GetBrowserContext(), origin, std::move(allow), {},
      base::BindOnce(
          [](const base::android::ScopedJavaGlobalRef<jobject>& done) {
            base::android::RunBooleanCallbackAndroid(done, true);
          },
          base::android::ScopedJavaGlobalRef<jobject>(jdone)));
}

}  // namespace wolvic
