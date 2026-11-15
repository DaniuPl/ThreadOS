/* The FreeType modules ThreadOS builds (Makefile, FT_SRC): TrueType and
   CFF outlines in sfnt containers (WOFF too, through zlib), the hinters
   they use, and the anti-aliasing renderer. FreeType reads this list in
   place of its own include/freetype/config/ftmodule.h, which names every
   module there is (FT_CONFIG_MODULES_H). */
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
