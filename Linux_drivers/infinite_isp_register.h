#define uint32_t unsigned int

union REG_ISP_TOP_EN {
    struct {
        uint32_t TOP_EN_DPC_EN       : 1;
        uint32_t TOP_EN_BLC_EN       : 1;
        uint32_t TOP_EN_LINEAR_EN    : 1;
        uint32_t TOP_EN_OECF_EN      : 1;
        uint32_t TOP_EN_DGAIN_EN     : 1;
        uint32_t TOP_EN_LSC_EN       : 1;
        uint32_t TOP_EN_BNR_EN       : 1;
        uint32_t TOP_EN_WB_EN        : 1;
        uint32_t TOP_EN_DEMOSIC_EN   : 1;
        uint32_t TOP_EN_CCM_EN       : 1;
        uint32_t TOP_EN_GAMMA_EN     : 1;
        uint32_t TOP_EN_CSC_EN       : 1;
        uint32_t TOP_EN_LDCI_EN      : 1;
        uint32_t TOP_EN_2DNR_EN      : 1;
        uint32_t TOP_EN_SHARP_EN     : 1;
        uint32_t TOP_EN_AE_EN        : 1;
        uint32_t TOP_EN_AWB_EN       : 1;
        uint32_t TOP_EN_CROP_EN      : 1;
        uint32_t TOP_EN_Reserved     : 14;
    };
    uint32_t TOP_EN_val;
};


union REG_VIP_TOP_EN {
    struct {
        uint32_t VIP_TOP_EN_RGBC_EN              : 1;
        uint32_t VIP_TOP_EN_IRC_EN               : 1;
        uint32_t VIP_TOP_EN_SCALE_EN             : 1;
        uint32_t VIP_TOP_EN_OSD_EN               : 1;
        uint32_t VIP_TOP_EN_YUVConvFormat_EN     : 1;
        uint32_t VIP_TOP_EN_Reserved             : 27;
    };
    uint32_t VIP_TOP_EN_val;
};

struct REG_CONFIG {
    uint32_t RESET;
    uint32_t SNS_WIDTH;
    uint32_t SNS_HEIGHT;
    uint32_t TARGET_CROP_WIDTH;
    uint32_t TARGET_CROP_HEIGHT;
    uint32_t BITS;
    uint32_t BAYER;
    uint32_t reserved_0[9];
    union REG_ISP_TOP_EN TOP_EN;
    uint32_t INT_STATUS;
    uint32_t INT_MASK;
    uint32_t reserved_1[109];
};

struct REG_DPC {
    uint32_t DPC_THRESHOLD;
    uint32_t reserved_2[127];
};

struct REG_BLC {
    uint32_t BLC_R;
    uint32_t BLC_GR;
    uint32_t BLC_GB;
    uint32_t BLC_B;
    uint32_t LINEAR_R;
    uint32_t LINEAR_GR;
    uint32_t LINEAR_GB;
    uint32_t LINEAR_B;
    uint32_t reserved_3[120];
};

struct REG_AE {
    uint32_t center_illuminance;
    uint32_t skewness;
    uint32_t ae_crop_left;
    uint32_t ae_crop_right;
    uint32_t ae_crop_top;
    uint32_t ae_crop_bottom;
    uint32_t ae_response;
    uint32_t ae_result_skewness;
    uint32_t ae_response_debug;
    uint32_t ae_done;
    uint32_t reserved_4[118];
};

struct REG_DGAIN {
    uint32_t dgain_isManual;
    uint32_t dgain_man_index;
    uint32_t dgain_index_out;
    uint32_t reserved_5[13];
    uint32_t dgain_array_0;
    uint32_t dgain_array_1;
    uint32_t dgain_array_2;
    uint32_t dgain_array_3;
    uint32_t dgain_array_4;
    uint32_t dgain_array_5;
    uint32_t dgain_array_6;
    uint32_t dgain_array_7;
    uint32_t dgain_array_8;
    uint32_t dgain_array_9;
    uint32_t dgain_array_10;
    uint32_t dgain_array_11;
    uint32_t dgain_array_12;
    uint32_t dgain_array_13;
    uint32_t dgain_array_14;
    uint32_t dgain_array_15;
    uint32_t dgain_array_16;
    uint32_t dgain_array_17;
    uint32_t dgain_array_18;
    uint32_t dgain_array_19;
    uint32_t dgain_array_20;
    uint32_t dgain_array_21;
    uint32_t dgain_array_22;
    uint32_t dgain_array_23;
    uint32_t dgain_array_24;
    uint32_t dgain_array_25;
    uint32_t dgain_array_26;
    uint32_t dgain_array_27;
    uint32_t dgain_array_28;
    uint32_t dgain_array_29;
    uint32_t dgain_array_30;
    uint32_t dgain_array_31;
    uint32_t dgain_array_32;
    uint32_t dgain_array_33;
    uint32_t dgain_array_34;
    uint32_t dgain_array_35;
    uint32_t dgain_array_36;
    uint32_t dgain_array_37;
    uint32_t dgain_array_38;
    uint32_t dgain_array_39;
    uint32_t dgain_array_40;
    uint32_t dgain_array_41;
    uint32_t dgain_array_42;
    uint32_t dgain_array_43;
    uint32_t dgain_array_44;
    uint32_t dgain_array_45;
    uint32_t dgain_array_46;
    uint32_t dgain_array_47;
    uint32_t dgain_array_48;
    uint32_t dgain_array_49;
    uint32_t dgain_array_50;
    uint32_t dgain_array_51;
    uint32_t dgain_array_52;
    uint32_t dgain_array_53;
    uint32_t dgain_array_54;
    uint32_t dgain_array_55;
    uint32_t dgain_array_56;
    uint32_t dgain_array_57;
    uint32_t dgain_array_58;
    uint32_t dgain_array_59;
    uint32_t dgain_array_60;
    uint32_t dgain_array_61;
    uint32_t dgain_array_62;
    uint32_t dgain_array_63;
    uint32_t dgain_array_64;
    uint32_t dgain_array_65;
    uint32_t dgain_array_66;
    uint32_t dgain_array_67;
    uint32_t dgain_array_68;
    uint32_t dgain_array_69;
    uint32_t dgain_array_70;
    uint32_t dgain_array_71;
    uint32_t dgain_array_72;
    uint32_t dgain_array_73;
    uint32_t dgain_array_74;
    uint32_t dgain_array_75;
    uint32_t dgain_array_76;
    uint32_t dgain_array_77;
    uint32_t dgain_array_78;
    uint32_t dgain_array_79;
    uint32_t dgain_array_80;
    uint32_t dgain_array_81;
    uint32_t dgain_array_82;
    uint32_t dgain_array_83;
    uint32_t dgain_array_84;
    uint32_t dgain_array_85;
    uint32_t dgain_array_86;
    uint32_t dgain_array_87;
    uint32_t dgain_array_88;
    uint32_t dgain_array_89;
    uint32_t dgain_array_90;
    uint32_t dgain_array_91;
    uint32_t dgain_array_92;
    uint32_t dgain_array_93;
    uint32_t dgain_array_94;
    uint32_t dgain_array_95;
    uint32_t dgain_array_96;
    uint32_t dgain_array_97;
    uint32_t dgain_array_98;
    uint32_t dgain_array_99;
    uint32_t reserved_6[12];
};

struct REG_LSC {
    uint32_t reserved_7[128];
};

struct REG_AWB {
    uint32_t AWB_UNDEREXPOSED_LIMIT;
    uint32_t AWB_OVEREXPOSED_LIMIT;
    uint32_t AWB_FRAMES;
    uint32_t FINAL_RGAIN;
    uint32_t FINAL_BGAIN;
    uint32_t reserved_8[123];
};

struct REG_WB {
    uint32_t WB_RGAIN;
    uint32_t WB_BGAIN;
    uint32_t reserved_9[126];
};

struct REG_CFA {
    uint32_t reserved_10[128];
};

struct REG_CCM {
    uint32_t ccm_rr;
    uint32_t ccm_rg;
    uint32_t ccm_rb;
    uint32_t ccm_gr;
    uint32_t ccm_gg;
    uint32_t ccm_gb;
    uint32_t ccm_br;
    uint32_t ccm_bg;
    uint32_t ccm_bb;
    uint32_t reserved_11[119];
};

struct REG_CSC {
    uint32_t csc_conv_std;
    uint32_t reserved_12[127];
};

struct REG_LDCI {
    uint32_t reserved_13[128];
};

struct REG_Reserved_0 {
    uint32_t reserved_14[256];
};

struct REG_SHARP {
    uint32_t sharpen_strength;
    uint32_t reserved_15[15];
    uint32_t luma_kernel_00;
    uint32_t luma_kernel_01;
    uint32_t luma_kernel_02;
    uint32_t luma_kernel_03;
    uint32_t luma_kernel_04;
    uint32_t luma_kernel_05;
    uint32_t luma_kernel_06;
    uint32_t luma_kernel_07;
    uint32_t luma_kernel_08;
    uint32_t luma_kernel_10;
    uint32_t luma_kernel_11;
    uint32_t luma_kernel_12;
    uint32_t luma_kernel_13;
    uint32_t luma_kernel_14;
    uint32_t luma_kernel_15;
    uint32_t luma_kernel_16;
    uint32_t luma_kernel_17;
    uint32_t luma_kernel_18;
    uint32_t luma_kernel_20;
    uint32_t luma_kernel_21;
    uint32_t luma_kernel_22;
    uint32_t luma_kernel_23;
    uint32_t luma_kernel_24;
    uint32_t luma_kernel_25;
    uint32_t luma_kernel_26;
    uint32_t luma_kernel_27;
    uint32_t luma_kernel_28;
    uint32_t luma_kernel_30;
    uint32_t luma_kernel_31;
    uint32_t luma_kernel_32;
    uint32_t luma_kernel_33;
    uint32_t luma_kernel_34;
    uint32_t luma_kernel_35;
    uint32_t luma_kernel_36;
    uint32_t luma_kernel_37;
    uint32_t luma_kernel_38;
    uint32_t luma_kernel_40;
    uint32_t luma_kernel_41;
    uint32_t luma_kernel_42;
    uint32_t luma_kernel_43;
    uint32_t luma_kernel_44;
    uint32_t luma_kernel_45;
    uint32_t luma_kernel_46;
    uint32_t luma_kernel_47;
    uint32_t luma_kernel_48;
    uint32_t luma_kernel_50;
    uint32_t luma_kernel_51;
    uint32_t luma_kernel_52;
    uint32_t luma_kernel_53;
    uint32_t luma_kernel_54;
    uint32_t luma_kernel_55;
    uint32_t luma_kernel_56;
    uint32_t luma_kernel_57;
    uint32_t luma_kernel_58;
    uint32_t luma_kernel_60;
    uint32_t luma_kernel_61;
    uint32_t luma_kernel_62;
    uint32_t luma_kernel_63;
    uint32_t luma_kernel_64;
    uint32_t luma_kernel_65;
    uint32_t luma_kernel_66;
    uint32_t luma_kernel_67;
    uint32_t luma_kernel_68;
    uint32_t luma_kernel_70;
    uint32_t luma_kernel_71;
    uint32_t luma_kernel_72;
    uint32_t luma_kernel_73;
    uint32_t luma_kernel_74;
    uint32_t luma_kernel_75;
    uint32_t luma_kernel_76;
    uint32_t luma_kernel_77;
    uint32_t luma_kernel_78;
    uint32_t luma_kernel_80;
    uint32_t luma_kernel_81;
    uint32_t luma_kernel_82;
    uint32_t luma_kernel_83;
    uint32_t luma_kernel_84;
    uint32_t luma_kernel_85;
    uint32_t luma_kernel_86;
    uint32_t luma_kernel_87;
    uint32_t luma_kernel_88;
    uint32_t reserved_16[31];
};

struct REG_Reserved_1 {
    uint32_t reserved_17[128];
};

struct REG_BNR {
    union {
        struct {
            uint32_t bnr_space_kernel_r00 : 8;
            uint32_t bnr_space_kernel_r01 : 8;
            uint32_t bnr_space_kernel_r02 : 8;
            uint32_t bnr_space_kernel_r03 : 8;
        };
        uint32_t union_val_0;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_r04 : 8;
            uint32_t Res_20 : 8;
            uint32_t Res_19 : 8;
            uint32_t Res_18 : 8;
        };
        uint32_t union_val_1;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_r10 : 8;
            uint32_t bnr_space_kernel_r11 : 8;
            uint32_t bnr_space_kernel_r12 : 8;
            uint32_t bnr_space_kernel_r13 : 8;
        };
        uint32_t union_val_2;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_r14 : 8;
            uint32_t Res_23 : 8;
            uint32_t Res_22 : 8;
            uint32_t Res_21 : 8;
        };
        uint32_t union_val_3;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_r20 : 8;
            uint32_t bnr_space_kernel_r21 : 8;
            uint32_t bnr_space_kernel_r22 : 8;
            uint32_t bnr_space_kernel_r23 : 8;
        };
        uint32_t union_val_4;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_r24 : 8;
            uint32_t Res_26 : 8;
            uint32_t Res_25 : 8;
            uint32_t Res_24 : 8;
        };
        uint32_t union_val_5;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_r30 : 8;
            uint32_t bnr_space_kernel_r31 : 8;
            uint32_t bnr_space_kernel_r32 : 8;
            uint32_t bnr_space_kernel_r33 : 8;
        };
        uint32_t union_val_6;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_r34 : 8;
            uint32_t Res_29 : 8;
            uint32_t Res_28 : 8;
            uint32_t Res_27 : 8;
        };
        uint32_t union_val_7;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_r40 : 8;
            uint32_t bnr_space_kernel_r41 : 8;
            uint32_t bnr_space_kernel_r42 : 8;
            uint32_t bnr_space_kernel_r43 : 8;
        };
        uint32_t union_val_8;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_r44 : 8;
            uint32_t Res_32 : 8;
            uint32_t Res_31 : 8;
            uint32_t Res_30 : 8;
        };
        uint32_t union_val_9;
    };
    uint32_t reserved_18[6];
    union {
        struct {
            uint32_t bnr_space_kernel_g00 : 8;
            uint32_t bnr_space_kernel_g01 : 8;
            uint32_t bnr_space_kernel_g02 : 8;
            uint32_t bnr_space_kernel_g03 : 8;
        };
        uint32_t union_val_10;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_g04 : 8;
            uint32_t Res_36 : 8;
            uint32_t Res_35 : 8;
            uint32_t Res_34 : 8;
        };
        uint32_t union_val_11;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_g10 : 8;
            uint32_t bnr_space_kernel_g11 : 8;
            uint32_t bnr_space_kernel_g12 : 8;
            uint32_t bnr_space_kernel_g13 : 8;
        };
        uint32_t union_val_12;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_g14 : 8;
            uint32_t Res_39 : 8;
            uint32_t Res_38 : 8;
            uint32_t Res_37 : 8;
        };
        uint32_t union_val_13;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_g20 : 8;
            uint32_t bnr_space_kernel_g21 : 8;
            uint32_t bnr_space_kernel_g22 : 8;
            uint32_t bnr_space_kernel_g23 : 8;
        };
        uint32_t union_val_14;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_g24 : 8;
            uint32_t Res_42 : 8;
            uint32_t Res_41 : 8;
            uint32_t Res_40 : 8;
        };
        uint32_t union_val_15;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_g30 : 8;
            uint32_t bnr_space_kernel_g31 : 8;
            uint32_t bnr_space_kernel_g32 : 8;
            uint32_t bnr_space_kernel_g33 : 8;
        };
        uint32_t union_val_16;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_g34 : 8;
            uint32_t Res_45 : 8;
            uint32_t Res_44 : 8;
            uint32_t Res_43 : 8;
        };
        uint32_t union_val_17;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_g40 : 8;
            uint32_t bnr_space_kernel_g41 : 8;
            uint32_t bnr_space_kernel_g42 : 8;
            uint32_t bnr_space_kernel_g43 : 8;
        };
        uint32_t union_val_18;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_g44 : 8;
            uint32_t Res_48 : 8;
            uint32_t Res_47 : 8;
            uint32_t Res_46 : 8;
        };
        uint32_t union_val_19;
    };
    uint32_t reserved_19[6];
    union {
        struct {
            uint32_t bnr_space_kernel_b00 : 8;
            uint32_t bnr_space_kernel_b01 : 8;
            uint32_t bnr_space_kernel_b02 : 8;
            uint32_t bnr_space_kernel_b03 : 8;
        };
        uint32_t union_val_20;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_b04 : 8;
            uint32_t Res_52 : 8;
            uint32_t Res_51 : 8;
            uint32_t Res_50 : 8;
        };
        uint32_t union_val_21;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_b10 : 8;
            uint32_t bnr_space_kernel_b11 : 8;
            uint32_t bnr_space_kernel_b12 : 8;
            uint32_t bnr_space_kernel_b13 : 8;
        };
        uint32_t union_val_22;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_b14 : 8;
            uint32_t Res_55 : 8;
            uint32_t Res_54 : 8;
            uint32_t Res_53 : 8;
        };
        uint32_t union_val_23;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_b20 : 8;
            uint32_t bnr_space_kernel_b21 : 8;
            uint32_t bnr_space_kernel_b22 : 8;
            uint32_t bnr_space_kernel_b23 : 8;
        };
        uint32_t union_val_24;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_b24 : 8;
            uint32_t Res_58 : 8;
            uint32_t Res_57 : 8;
            uint32_t Res_56 : 8;
        };
        uint32_t union_val_25;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_b30 : 8;
            uint32_t bnr_space_kernel_b31 : 8;
            uint32_t bnr_space_kernel_b32 : 8;
            uint32_t bnr_space_kernel_b33 : 8;
        };
        uint32_t union_val_26;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_b34 : 8;
            uint32_t Res_61 : 8;
            uint32_t Res_60 : 8;
            uint32_t Res_59 : 8;
        };
        uint32_t union_val_27;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_b40 : 8;
            uint32_t bnr_space_kernel_b41 : 8;
            uint32_t bnr_space_kernel_b42 : 8;
            uint32_t bnr_space_kernel_b43 : 8;
        };
        uint32_t union_val_28;
    };
    union {
        struct {
            uint32_t bnr_space_kernel_b44 : 8;
            uint32_t Res_64 : 8;
            uint32_t Res_63 : 8;
            uint32_t Res_62 : 8;
        };
        uint32_t union_val_29;
    };
    uint32_t reserved_20[22];
    union {
        struct {
            uint32_t bnr_color_curve_x_r_0 : 16;
            uint32_t bnr_color_curve_y_r_0 : 16;
        };
        uint32_t union_val_30;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_r_1 : 16;
            uint32_t bnr_color_curve_y_r_1 : 16;
        };
        uint32_t union_val_31;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_r_2 : 16;
            uint32_t bnr_color_curve_y_r_2 : 16;
        };
        uint32_t union_val_32;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_r_3 : 16;
            uint32_t bnr_color_curve_y_r_3 : 16;
        };
        uint32_t union_val_33;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_r_4 : 16;
            uint32_t bnr_color_curve_y_r_4 : 16;
        };
        uint32_t union_val_34;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_r_5 : 16;
            uint32_t bnr_color_curve_y_r_5 : 16;
        };
        uint32_t union_val_35;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_r_6 : 16;
            uint32_t bnr_color_curve_y_r_6 : 16;
        };
        uint32_t union_val_36;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_r_7 : 16;
            uint32_t bnr_color_curve_y_r_7 : 16;
        };
        uint32_t union_val_37;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_r_8 : 16;
            uint32_t bnr_color_curve_y_r_8 : 16;
        };
        uint32_t union_val_38;
    };
    uint32_t reserved_21[7];
    union {
        struct {
            uint32_t bnr_color_curve_x_g_0 : 16;
            uint32_t bnr_color_curve_y_g_0 : 16;
        };
        uint32_t union_val_39;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_g_1 : 16;
            uint32_t bnr_color_curve_y_g_1 : 16;
        };
        uint32_t union_val_40;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_g_2 : 16;
            uint32_t bnr_color_curve_y_g_2 : 16;
        };
        uint32_t union_val_41;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_g_3 : 16;
            uint32_t bnr_color_curve_y_g_3 : 16;
        };
        uint32_t union_val_42;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_g_4 : 16;
            uint32_t bnr_color_curve_y_g_4 : 16;
        };
        uint32_t union_val_43;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_g_5 : 16;
            uint32_t bnr_color_curve_y_g_5 : 16;
        };
        uint32_t union_val_44;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_g_6 : 16;
            uint32_t bnr_color_curve_y_g_6 : 16;
        };
        uint32_t union_val_45;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_g_7 : 16;
            uint32_t bnr_color_curve_y_g_7 : 16;
        };
        uint32_t union_val_46;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_g_8 : 16;
            uint32_t bnr_color_curve_y_g_8 : 16;
        };
        uint32_t union_val_47;
    };
    uint32_t reserved_22[7];
    union {
        struct {
            uint32_t bnr_color_curve_x_b_0 : 16;
            uint32_t bnr_color_curve_y_b_0 : 16;
        };
        uint32_t union_val_48;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_b_1 : 16;
            uint32_t bnr_color_curve_y_b_1 : 16;
        };
        uint32_t union_val_49;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_b_2 : 16;
            uint32_t bnr_color_curve_y_b_2 : 16;
        };
        uint32_t union_val_50;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_b_3 : 16;
            uint32_t bnr_color_curve_y_b_3 : 16;
        };
        uint32_t union_val_51;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_b_4 : 16;
            uint32_t bnr_color_curve_y_b_4 : 16;
        };
        uint32_t union_val_52;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_b_5 : 16;
            uint32_t bnr_color_curve_y_b_5 : 16;
        };
        uint32_t union_val_53;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_b_6 : 16;
            uint32_t bnr_color_curve_y_b_6 : 16;
        };
        uint32_t union_val_54;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_b_7 : 16;
            uint32_t bnr_color_curve_y_b_7 : 16;
        };
        uint32_t union_val_55;
    };
    union {
        struct {
            uint32_t bnr_color_curve_x_b_8 : 16;
            uint32_t bnr_color_curve_y_b_8 : 16;
        };
        uint32_t union_val_56;
    };
    uint32_t reserved_23[23];
};

struct REG_Reserved_2 {
    uint32_t reserved_24[512];
};

struct REG_2DNR {
    union {
        struct {
            uint32_t nr2d_diff_0 : 8;
            uint32_t nr2d_diff_1 : 8;
            uint32_t nr2d_diff_2 : 8;
            uint32_t nr2d_diff_3 : 8;
        };
        uint32_t union_val_57;
    };
    union {
        struct {
            uint32_t nr2d_diff_4 : 8;
            uint32_t nr2d_diff_5 : 8;
            uint32_t nr2d_diff_6 : 8;
            uint32_t nr2d_diff_7 : 8;
        };
        uint32_t union_val_58;
    };
    union {
        struct {
            uint32_t nr2d_diff_8 : 8;
            uint32_t nr2d_diff_9 : 8;
            uint32_t nr2d_diff_10 : 8;
            uint32_t nr2d_diff_11 : 8;
        };
        uint32_t union_val_59;
    };
    union {
        struct {
            uint32_t nr2d_diff_12 : 8;
            uint32_t nr2d_diff_13 : 8;
            uint32_t nr2d_diff_14 : 8;
            uint32_t nr2d_diff_15 : 8;
        };
        uint32_t union_val_60;
    };
    union {
        struct {
            uint32_t nr2d_diff_16 : 8;
            uint32_t nr2d_diff_17 : 8;
            uint32_t nr2d_diff_18 : 8;
            uint32_t nr2d_diff_19 : 8;
        };
        uint32_t union_val_61;
    };
    union {
        struct {
            uint32_t nr2d_diff_20 : 8;
            uint32_t nr2d_diff_21 : 8;
            uint32_t nr2d_diff_22 : 8;
            uint32_t nr2d_diff_23 : 8;
        };
        uint32_t union_val_62;
    };
    union {
        struct {
            uint32_t nr2d_diff_24 : 8;
            uint32_t nr2d_diff_25 : 8;
            uint32_t nr2d_diff_26 : 8;
            uint32_t nr2d_diff_27 : 8;
        };
        uint32_t union_val_63;
    };
    union {
        struct {
            uint32_t nr2d_diff_28 : 8;
            uint32_t nr2d_diff_29 : 8;
            uint32_t nr2d_diff_30 : 8;
            uint32_t nr2d_diff_31 : 8;
        };
        uint32_t union_val_64;
    };
    uint32_t reserved_25[8];
    union {
        struct {
            uint32_t nr2d_weight_0 : 8;
            uint32_t nr2d_weight_1 : 8;
            uint32_t nr2d_weight_2 : 8;
            uint32_t nr2d_weight_3 : 8;
        };
        uint32_t union_val_65;
    };
    union {
        struct {
            uint32_t nr2d_weight_4 : 8;
            uint32_t nr2d_weight_5 : 8;
            uint32_t nr2d_weight_6 : 8;
            uint32_t nr2d_weight_7 : 8;
        };
        uint32_t union_val_66;
    };
    union {
        struct {
            uint32_t nr2d_weight_8 : 8;
            uint32_t nr2d_weight_9 : 8;
            uint32_t nr2d_weight_10 : 8;
            uint32_t nr2d_weight_11 : 8;
        };
        uint32_t union_val_67;
    };
    union {
        struct {
            uint32_t nr2d_weight_12 : 8;
            uint32_t nr2d_weight_13 : 8;
            uint32_t nr2d_weight_14 : 8;
            uint32_t nr2d_weight_15 : 8;
        };
        uint32_t union_val_68;
    };
    union {
        struct {
            uint32_t nr2d_weight_16 : 8;
            uint32_t nr2d_weight_17 : 8;
            uint32_t nr2d_weight_18 : 8;
            uint32_t nr2d_weight_19 : 8;
        };
        uint32_t union_val_69;
    };
    union {
        struct {
            uint32_t nr2d_weight_20 : 8;
            uint32_t nr2d_weight_21 : 8;
            uint32_t nr2d_weight_22 : 8;
            uint32_t nr2d_weight_23 : 8;
        };
        uint32_t union_val_70;
    };
    union {
        struct {
            uint32_t nr2d_weight_24 : 8;
            uint32_t nr2d_weight_25 : 8;
            uint32_t nr2d_weight_26 : 8;
            uint32_t nr2d_weight_27 : 8;
        };
        uint32_t union_val_71;
    };
    union {
        struct {
            uint32_t nr2d_weight_28 : 8;
            uint32_t nr2d_weight_29 : 8;
            uint32_t nr2d_weight_30 : 8;
            uint32_t nr2d_weight_31 : 8;
        };
        uint32_t union_val_72;
    };
    uint32_t reserved_26[104];
};

struct REG_Reserved_3 {
    uint32_t reserved_27[1280];
};

struct REG_VIP_CONFIG {
    uint32_t VIP_RESET;
    uint32_t VIP_WIDTH;
    uint32_t VIP_HEIGHT;
    uint32_t VIP_BITS;
    uint32_t reserved_28[12];
    union REG_VIP_TOP_EN VIP_TOP_EN;
    uint32_t VIP_INT_STATUS;
    uint32_t VIP_INT_MASK;
    uint32_t reserved_29[109];
};

struct REG_RGBC {
    uint32_t in_conv_standard;
    uint32_t reserved_30[127];
};

struct REG_IRC {
    uint32_t CROP_X;
    uint32_t CROP_Y;
    uint32_t IRC_OUTPUT;
    uint32_t reserved_31[125];
};

struct REG_SCALE {
    uint32_t s_in_crop_w;
    uint32_t s_in_crop_h;
    uint32_t s_out_crop_w;
    uint32_t s_out_crop_h;
    uint32_t dscale_w;
    uint32_t dscale_h;
    uint32_t reserved_32[122];
};

struct REG_OSD {
    uint32_t OSD_X;
    uint32_t OSD_Y;
    uint32_t OSD_W;
    uint32_t OSD_H;
    union {
        struct {
            uint32_t OSD_COLOR_FG_B : 8;
            uint32_t OSD_COLOR_FG_G : 8;
            uint32_t OSD_COLOR_FG_R : 8;
            uint32_t Reserved_78 : 8;
        };
        uint32_t union_val_73;
    };
    union {
        struct {
            uint32_t OSD_COLOR_BG_B : 8;
            uint32_t OSD_COLOR_BG_G : 8;
            uint32_t OSD_COLOR_BG_R : 8;
            uint32_t Reserved_79 : 8;
        };
        uint32_t union_val_74;
    };
    uint32_t ALPHA;
    uint32_t reserved_33[121];
};

struct REG_YUVConvFormat {
    uint32_t YUV444TO422;
    uint32_t reserved_34[127];
};

struct REG_Reserved_4 {
    uint32_t reserved_35[1280];
};

struct REG_GAMMA_LUT {
    uint32_t GAMMA_LUT[4096];
};

struct REG_VIP1_OSD_RAM {
    uint32_t VIP1_OSD_RAM[512];
    uint32_t reserved_44[1536];
};

struct REG_VIP2_OSD_RAM {
    uint32_t VIP2_OSD_RAM[512];
    uint32_t reserved_45[1536];
};

struct REG_OECF_LUTs {
    uint32_t OECF_R_LUT[4096];
    uint32_t OECF_GR_LUT[4096];
    uint32_t OECF_GB_LUT[4096];
    uint32_t OECF_B_LUT[4096];
};

struct REG_Infinite_ISP {
    struct REG_CONFIG config;
    struct REG_DPC dpc;
    struct REG_BLC blc;
    struct REG_AE ae;
    struct REG_DGAIN dgain;
    struct REG_LSC lsc;
    struct REG_AWB awb;
    struct REG_WB wb;
    struct REG_CFA cfa;
    struct REG_CCM ccm;
    struct REG_CSC csc;
    struct REG_LDCI ldci;
    struct REG_Reserved_0 reserved_0;
    struct REG_SHARP sharp;
    struct REG_Reserved_1 reserved_1;
    struct REG_BNR bnr;
    struct REG_Reserved_2 reserved_2;
    struct REG_2DNR _2dnr;
    // struct REG_Reserved_3 reserved_3;
};

struct REG_Infinite_ISP_VIP {
    struct REG_VIP_CONFIG vip_config;
    struct REG_RGBC rgbc;
    struct REG_IRC irc;
    struct REG_SCALE scale;
    struct REG_OSD osd;
    struct REG_YUVConvFormat yuvconvformat;
    // struct REG_Reserved_4 reserved_4;
};

struct REG_Infinite_ISP_LUT {
    struct REG_GAMMA_LUT gamma_lut;
    struct REG_VIP1_OSD_RAM vip1_osd_ram;
    struct REG_VIP2_OSD_RAM vip2_osd_ram;
    struct REG_OECF_LUTs oecf_luts;
};


#define INFINITE_ISP_READ_REG(iommu, module_name, register_name) \
    ioread32(&((volatile struct REG_Infinite_ISP __iomem *)(iommu))->module_name.register_name)

#define INFINITE_ISP_WRITE_REG(iommu, module_name, register_name, value) \
    iowrite32((value), &((volatile struct REG_Infinite_ISP __iomem *)(iommu))->module_name.register_name)

#define INFINITE_ISP_VIP_READ_REG(iommu, module_name, register_name) \
	ioread32(&((volatile struct REG_Infinite_ISP_VIP __iomem *)(iommu))->module_name.register_name)
#define INFINITE_ISP_VIP_WRITE_REG(iommu, module_name, register_name, value) \
    iowrite32((value), &((volatile struct REG_Infinite_ISP_VIP __iomem *)(iommu))->module_name.register_name)

#define INFINITE_MODE_READ_MODULE_REGs(iommu, struct_name, module_name, buffer) \
    memcpy_fromio(buffer, \
        (void __iomem *)((char __iomem *)(iommu) + offsetof(struct struct_name, module_name)), \
        sizeof(((struct struct_name *)0)->module_name))

#define INFINITE_MODE_WRITE_MODULE_REGs(iommu, struct_name, module_name, buffer) \
    memcpy_toio( \
        (void __iomem *)((char __iomem *)(iommu) + offsetof(struct struct_name, module_name)), \
        buffer, \
        sizeof(((struct struct_name *)0)->module_name))

#define INFINITE_ISP_READ_MODULE_REGs(iommu, module_name, buffer) \
	INFINITE_MODE_READ_MODULE_REGs(iommu, REG_Infinite_ISP, module_name, buffer)

#define INFINITE_ISP_WRITE_MODULE_REGs(iommu, module_name, buffer) \
	INFINITE_MODE_WRITE_MODULE_REGs(iommu, REG_Infinite_ISP, module_name, buffer)

#define INFINITE_ISP_READ_VIP_REGs(iommu, module_name, buffer) \
	INFINITE_MODE_READ_MODULE_REGs(iommu, REG_Infinite_ISP_VIP, module_name, buffer)

#define INFINITE_ISP_WRITE_VIP_REGs(iommu, module_name, buffer) \
	INFINITE_MODE_WRITE_MODULE_REGs(iommu, REG_Infinite_ISP_VIP, module_name, buffer)

#define INFINITE_ISP_READ_LUT_REGs(iommu, module_name, buffer) \
	INFINITE_MODE_READ_MODULE_REGs(iommu, REG_Infinite_ISP_LUT, module_name, buffer)

#define INFINITE_ISP_WRITE_LUT_REGs(iommu, module_name, buffer) \
	INFINITE_MODE_WRITE_MODULE_REGs(iommu, REG_Infinite_ISP_LUT, module_name, buffer)

