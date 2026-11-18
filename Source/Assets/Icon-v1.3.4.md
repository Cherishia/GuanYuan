# 关源 v1.3.4 图标

使用内置 image_gen 编辑模式（identity-preserve），未使用 CLI 回退。

用户要求：只参考金发头像的构图，保留红白巫女和原来的红底。

输入：旧版红白巫女头像、用户最初的红白巫女原图、用户新提供的金发头像构图参考。

输出：portrait.png 为新生成原图；AppIcon.png、app.ico 仅做尺寸和格式转换。另使用内置 image_gen 提取透明前景 portrait-foreground.png，提示词见 Icon-foreground-prompt.md；保留 alpha 缩放为 Android ic_power.png。Android 图层内缩为 22.22%，背景保留原来的 #A2262E，以保留圆形图标下的完整蝴蝶结，并去除内部方框。预览使用真实图层几何及代表性桌面蒙版渲染，不代表已在实体手机上安装。

仅更新图标、版本号和页头版本文字，程序逻辑与 v1.3.3 一致，保留连接时间校准修复。

最终完整提示词：

Use case: identity-preserve. Asset type: square GuanYuan application icon master. Input image 1 is the current red shrine-maiden portrait to edit. Input image 2 is the original character, clothing and red background reference. Input image 3 (blonde girl on cream) is COMPOSITION ONLY, never its character or colors. Keep the red-white shrine maiden identity, dark brown hair, reddish brown eyes, large red-and-white hair bow, red-white clothing and yellow neck ribbon. Preserve her existing face, neutral gentle expression and painterly anime style. Recompose to match image 3's clear large head-and-shoulders avatar: visually centered, larger face, full bow visible with breathing room above, hair and shoulders naturally filling the lower portion. Portrait occupies roughly 80-85 percent canvas width, head-to-shoulders about 90 percent height. Leave about 6-9 percent red margin above the whole bow and 8-12 percent to either side of the head; keep face and complete bow within a central launcher-safe region. Shoulder silhouette should have a clean softly curved bottom edge near the bottom of canvas like reference 3, with a thin red margin below, rather than dissolving into a cloud or mist. Keep the background the original muted deep crimson RED throughout the entire square, with only very subtle red tonal variation. This is an improved crop/composition of the SAME girl, not a replacement or restyling. Absolutely no blonde hair, blue eyes, cream, beige, off-white background, extra props, text, logo, border, circle badge, baked-in rounded square, watermark, or foggy fade-out of torso. Produce one polished opaque square icon.

