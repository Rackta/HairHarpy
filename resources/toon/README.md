# HairHarpy Toon — экспорт шейдера

Папку записала HairHarpy: Materials & Shader → Export. Здесь тот же шейдер, что в режиме шейдера HairHarpy: одно ядро
`toon_core.glsl` (GLSL в HairHarpy и на сайте, HLSL в Unreal и Unity — текст один, без переписывания), карты каждого UV-сета
и параметры каждой части модели.

| Что | Где |
|---|---|
| Карты UV-сетов (Base colour, Shade colour, Shadow mask, Highlight, Rim, Emission, Normal, Flow, Outline width, Over hair, ID) | `maps/<сет>_<карта>.png` |
| Картинка тени лица (SDF), если задана в материале | `maps/sdf_*.png` |
| Материалы, инстансы, кто что носит, роли, локатор головы, упакованные параметры каждой части (`P`: 32 × float4) | `hh_toon.json` |
| Ядро шейдера | `toon_core.glsl` (для Unity — копия `unity/hh_toon_core.hlsl`) |
| Unreal Engine 5 | `unreal/hh_toon_import.py` |
| Сайт (three.js) | `web/hh_toon.js` |
| Unity (URP) | `unity/HHToon.shader`, `unity/Editor/HHToonImporter.cs` |

Карты нормалей — OpenGL (зелёный вверх по картинке). Строки картинок — как в HairHarpy: строка 0 — верх.
Части модели находятся по имени объекта HairHarpy и по имени слота материала = имя UV-сета (так их пишет экспорт FBX HairHarpy).

## Unreal Engine 5

1. Импортируйте FBX модели (слоты материалов = UV-сеты).
2. Включите плагин *Python Editor Script Plugin*; для бровей поверх волос — Project Settings → Rendering →
   *Custom Depth-Stencil Pass* = *Enabled with Stencil*.
3. Выделите актёров с моделью на уровне и запустите `Tools → Execute Python Script…` → `unreal/hh_toon_import.py`.

Скрипт создаёт в `/Game/HairHarpyToon/<проект>/`: текстуры, `M_HHToon` (Unlit, одна Custom-нода с ядром), `M_HHToon_OverHair`,
`M_HHToon_Outline`, инстансы `MI_<объект>_<сет>` и раздаёт их слотам выделенных актёров (обводка — в Overlay Material).
Свет — первый направленный свет неба (*SkyAtmosphereLightDirection*): у солнца должен стоять *Atmosphere Sun Light*.

- Если рельеф карты нормалей выглядит вывернутым — в инстансе поставьте `FlipGreen` = −1.
- Брови поверх волос: скрипт включает Custom Depth частям с ролью «поверх волос» (stencil 1) и остальным, кроме волос
  (stencil 2), и прикрепляет к актёру копию меша `<актёр>_HHOverHair` с инстансами `MIH_<объект>_<сет>`: она рисуется поверх
  волос только там, где брови закрыты волосами. Скелетная копия повторяет позу оригинала (Leader Pose). Повторный запуск
  скрипта пересоздаёт копии.
- Локатор головы переводится в оси Unreal (Z вверх) так же, как модель при импорте FBX.
- Проверено в UE 5.5, 5.7 и 5.8. В 5.5 Python не умеет подключать выход World Position Offset: скрипт напишет об этом
  в Output Log — откройте `M_HHToon_Outline` и соедините Custom-ноду с World Position Offset (один провод), иначе обводки не видно.
- Параметры `P00…P31` — упакованные значения HairHarpy (как в `hh_toon.json`). Меняйте их в HairHarpy и экспортируйте снова.

## Сайт (three.js r160+)

```js
import * as THREE from 'three';
import { loadHHToon } from './web/hh_toon.js';
const renderer = new THREE.WebGLRenderer({ antialias: true, stencil: true });
const hh = await loadHHToon(THREE, '<url этой папки>');   // модель из FBX (FBXLoader)
// модель из glTF (например, экспорт Blender): loadHHToon(THREE, '<url>', { gltf: true }) — у glTF V уже сверху вниз
hh.applyTo(model);
hh.setLight(new THREE.Vector3(0.4, 0.8, 0.5));
hh.setViewSize(innerWidth, innerHeight); // толщина обводки в пикселях
hh.setHead(headBone);                    // по желанию: кость головы (вперёд +Z, вверх +Y)
```

`hh_toon.js` грузит `hh_toon.json`, `toon_core.glsl` и карты из этой папки (по относительным путям), делает материал на каждый
слот, копию меша с обводкой и копию «поверх волос» для бровей (stencil). Поддерживает скиннинг.

## Unity (URP 14+)

1. Положите всю папку экспорта внутрь `Assets`.
2. Импортируйте модель, откройте сцену с ней.
3. Меню `HairHarpy → Import Toon (hh_toon.json)` → выберите `hh_toon.json`.

Импортёр выставляет картам sRGB / линейность, создаёт материалы `HH_<объект>_<сет>` (шейдер `HairHarpy/Toon`) и раздаёт их
рендерерам сцены. Части «поверх волос» получают второй материал (рисуется поверх волос по stencil); свет — главный свет URP.
Локатор головы отражается по X, как модель при импорте FBX в Unity.
