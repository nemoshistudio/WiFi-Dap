import {createRouter, createWebHashHistory, type RouteLocationNormalizedLoaded} from 'vue-router'
import Wifi from '@/views/Wifi.vue'
import TargetFlash from '@/views/TargetFlash.vue'
import Status from '@/views/Status.vue'
import DebugCurve from '@/views/DebugCurve.vue'
import About from '@/views/About.vue'
import Uart from '@/views/Uart.vue'
import Page404 from '@/views/404.vue'
import {translate} from "@/locales";
import {reactive, watch} from "vue";
import {getLang} from "@/i18n";
import {BRAND_NAME} from "@/constants/brand";

const languageState = reactive({
    lang: getLang()
});

interface AppRouteMeta {
    /** 页面标题 i18n key（docs/i18n-keys.md §2.3 / §5） */
    titleKey?: string;
    /** 由 updateMetaTitles() 计算出的当前语言标题（NavBar 移动端使用） */
    title?: string;
}

/**
 * 路由标题使用显式 switch 取 key：i18n 契约 §2.2 禁止动态拼接 key，
 * 这里按 route.name 静态映射到字面量 key（新增页面在此追加 case）。
 */
function translateRouteTitle(name: unknown): string {
    switch (name) {
        case 'home':
            return translate('page.home');
        case 'wifi':
            return translate('page.wifi');
        case 'uart':
            return translate('page.uart');
        case 'about':
            return translate('page.about');
        case 'targetFlash':
            return translate('target.title');
        case 'debugCurve':
            return translate('page.debugCurve');
        case 'status':
            return translate('page.status');
        default:
            // 404 / 未知路由回退品牌名（i18n 契约 §5：标题回退使用品牌常量）
            return BRAND_NAME;
    }
}

const updateMetaTitles = () => {
    router.getRoutes().forEach(route => {
        const meta = route.meta as AppRouteMeta;
        if (meta.titleKey) {
            meta.title = translateRouteTitle(route.name);
        }
    });
};

function updateDocumentTitle(route: RouteLocationNormalizedLoaded) {
    document.title = `${translateRouteTitle(route.name)} | ${BRAND_NAME}`;
}

// Watch for language changes to update the titles dynamically
watch(() => languageState.lang, () => {
    // Recompute all route meta titles
    updateMetaTitles();
    updateDocumentTitle(router.currentRoute.value);
}, {deep: true});

const router = createRouter({
    history: createWebHashHistory(import.meta.env.BASE_URL),
    routes: [
        {
            path: '/',
            name: 'home',
            meta: { titleKey: 'page.home' },
            // P0：首页即目标烧录页（`/target`）；`/` 只保留重定向，便于旧书签可用。
            redirect: () => '/target',
        }, {
            path: '/wifi:ext(.*)',
            meta: { titleKey: 'page.wifi' },
            name: 'wifi',
            component: Wifi,
        }, {
            path: '/uart:ext(.*)',
            meta: { titleKey: 'page.uart' },
            name: 'uart',
            component: Uart,
        }, {
            path: '/target:ext(.*)',
            meta: { titleKey: 'target.title' },
            name: 'targetFlash',
            component: TargetFlash,
        }, {
            path: '/status:ext(.*)',
            meta: { titleKey: 'page.status' },
            name: 'status',
            component: Status,
        }, {
            path: '/debug-curve:ext(.*)',
            meta: { titleKey: 'page.debugCurve' },
            name: 'debugCurve',
            component: DebugCurve,
        }, {
            path: '/about:ext(.*)',
            meta: { titleKey: 'page.about' },
            name: 'about',
            component: About,
        }, {
            path: '/:catchAll(.*)', // Catch-all route for 404
            name: 'NotFound',
            component: Page404,
        },
    ]
})

// Update document title dynamically
router.beforeEach((to, from, next) => {
    updateDocumentTitle(to);
    next();
});

// Initialize titles on load
updateMetaTitles();

export default router;
