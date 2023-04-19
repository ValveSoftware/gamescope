
vec4 textureCond(sampler2D layerSampler, uint layerIdx, vec2 pos, bool unnormalized) {
    vec2 texSize = textureSize(layerSampler, 0);
    vec2 coord = pos;
#ifndef BLUR_DONT_SCALE
    coord = ((coord + u_offset[layerIdx]) * u_scale[layerIdx]);

    if (coord.x < 0.0f       || coord.y < 0.0f ||
        coord.x >= texSize.x || coord.y >= texSize.y) {
        float border = (u_borderMask & (1u << layerIdx)) != 0 ? 1.0f : 0.0f;
        return vec4(0.0f, 0.0f, 0.0f, border);
    }
#endif

    if (!unnormalized)
        pos /= texSize;

    return textureLod(layerSampler, coord, 0.0f);
}

// everything except pos has to be at least spec constant
vec4 gaussian_blur(sampler2D layerSampler, uint layerIdx, vec2 pos, uint radius, bool vertical, bool unnormalized) {
    float offsets[20];
    float weights[20];
    int steps;
    if (radius <= 1) {
        steps = 1;
        weights[0] = 0.50000000;

        offsets[0] = 0.01741678;
    }
    else if (radius <= 3) {
        steps = 2;
        weights[0] = 0.44907984;
        weights[1] = 0.05092017;

        offsets[0] = 0.53804874;
        offsets[1] = 2.06277966;
    }
    else if (radius <= 5) {
        steps = 3;
        weights[0] = 0.33022788;
        weights[1] = 0.15701161;
        weights[2] = 0.01276049;

        offsets[0] = 0.62183881;
        offsets[1] = 2.27310348;
        offsets[2] = 4.14653015;
    }
    else if (radius <= 7) {
        steps = 4;
        weights[0] = 0.24961469;
        weights[1] = 0.19246334;
        weights[2] = 0.05147626;
        weights[3] = 0.00644572;

        offsets[0] = 0.64434171;
        offsets[1] = 2.37884760;
        offsets[2] = 4.29111052;
        offsets[3] = 6.21660709;
    }
    else if (radius <= 9) {
        steps = 5;
        weights[0] = 0.19954681;
        weights[1] = 0.18945214;
        weights[2] = 0.08376212;
        weights[3] = 0.02321143;
        weights[4] = 0.00402750;

        offsets[0] = 0.65318614;
        offsets[1] = 2.42546821;
        offsets[2] = 4.36803484;
        offsets[3] = 6.31411505;
        offsets[4] = 8.26478577;
    }
    else if (radius <= 11) {
        steps = 6;
        weights[0] = 0.16501401;
        weights[1] = 0.17507112;
        weights[2] = 0.10112059;
        weights[3] = 0.04267556;
        weights[4] = 0.01315655;
        weights[5] = 0.00296217;

        offsets[0] = 0.65771931;
        offsets[1] = 2.45016599;
        offsets[2] = 4.41095972;
        offsets[3] = 6.37285233;
        offsets[4] = 8.33626175;
        offsets[5] = 10.30153465;
    }
    else if (radius <= 13) {
        steps = 7;
        weights[0] = 0.14089632;
        weights[1] = 0.15927219;
        weights[2] = 0.10714546;
        weights[3] = 0.05746555;
        weights[4] = 0.02457064;
        weights[5] = 0.00837465;
        weights[6] = 0.00227519;

        offsets[0] = 0.66025251;
        offsets[1] = 2.46415234;
        offsets[2] = 4.43572092;
        offsets[3] = 6.40770626;
        offsets[4] = 8.38027859;
        offsets[5] = 10.35359478;
        offsets[6] = 12.32779312;
    }
    else if (radius <= 15) {
        steps = 8;
        weights[0] = 0.12248611;
        weights[1] = 0.14426580;
        weights[2] = 0.10711376;
        weights[3] = 0.06708494;
        weights[4] = 0.03544008;
        weights[5] = 0.01579223;
        weights[6] = 0.00593551;
        weights[7] = 0.00188158;

        offsets[0] = 0.66187197;
        offsets[1] = 2.47315121;
        offsets[2] = 4.45177603;
        offsets[3] = 6.43057728;
        offsets[4] = 8.40962982;
        offsets[5] = 10.38900566;
        offsets[6] = 12.36877155;
        offsets[7] = 14.34898853;
    }
    else if (radius <= 17) {
        steps = 9;
        weights[0] = 0.10855028;
        weights[1] = 0.13135010;
        weights[2] = 0.10405597;
        weights[3] = 0.07215953;
        weights[4] = 0.04380336;
        weights[5] = 0.02327578;
        weights[6] = 0.01082628;
        weights[7] = 0.00440784;
        weights[8] = 0.00157086;

        offsets[0] = 0.66292763;
        offsets[1] = 2.47903848;
        offsets[2] = 4.46231890;
        offsets[3] = 6.44568348;
        offsets[4] = 8.42916870;
        offsets[5] = 10.41281033;
        offsets[6] = 12.39664268;
        offsets[7] = 14.38069725;
        offsets[8] = 16.36500549;
    }
    else if (radius <= 19) {
        steps = 10;
        weights[0] = 0.09721643;
        weights[1] = 0.11994337;
        weights[2] = 0.09955716;
        weights[3] = 0.07429117;
        weights[4] = 0.04983896;
        weights[5] = 0.03005843;
        weights[6] = 0.01629773;
        weights[7] = 0.00794417;
        weights[8] = 0.00348120;
        weights[9] = 0.00137140;

        offsets[0] = 0.66368324;
        offsets[1] = 2.48326135;
        offsets[2] = 4.46989584;
        offsets[3] = 6.45657301;
        offsets[4] = 8.44331169;
        offsets[5] = 10.43013191;
        offsets[6] = 12.41704941;
        offsets[7] = 14.40408325;
        offsets[8] = 16.39124870;
        offsets[9] = 18.37856293;
    }
    else if (radius <= 21) {
        steps = 11;
        weights[0] = 0.08818824;
        weights[1] = 0.11032440;
        weights[2] = 0.09467436;
        weights[3] = 0.07444362;
        weights[4] = 0.05363600;
        weights[5] = 0.03540938;
        weights[6] = 0.02141965;
        weights[7] = 0.01187237;
        weights[8] = 0.00602965;
        weights[9] = 0.00280591;
        weights[10] = 0.00119642;

        offsets[0] = 0.66422051;
        offsets[1] = 2.48626900;
        offsets[2] = 4.47529793;
        offsets[3] = 6.46435070;
        offsets[4] = 8.45343781;
        offsets[5] = 10.44256973;
        offsets[6] = 12.43175602;
        offsets[7] = 14.42100716;
        offsets[8] = 16.41033173;
        offsets[9] = 18.39974213;
        offsets[10] = 20.38924408;
    }
    else if (radius <= 23) {
        steps = 12;
        weights[0] = 0.08053473;
        weights[1] = 0.10183022;
        weights[2] = 0.08965189;
        weights[3] = 0.07338920;
        weights[4] = 0.05585918;
        weights[5] = 0.03953177;
        weights[6] = 0.02601279;
        weights[7] = 0.01591534;
        weights[8] = 0.00905383;
        weights[9] = 0.00478890;
        weights[10] = 0.00235519;
        weights[11] = 0.00107696;

        offsets[0] = 0.66463250;
        offsets[1] = 2.48857713;
        offsets[2] = 4.47944689;
        offsets[3] = 6.47033072;
        offsets[4] = 8.46123409;
        offsets[5] = 10.45216274;
        offsets[6] = 12.44312382;
        offsets[7] = 14.43412209;
        offsets[8] = 16.42516327;
        offsets[9] = 18.41625404;
        offsets[10] = 20.40739822;
        offsets[11] = 22.39860344;
    }
    else if (radius <= 25) {
        steps = 13;
        weights[0] = 0.07409717;
        weights[1] = 0.09446181;
        weights[2] = 0.08481805;
        weights[3] = 0.07161362;
        weights[4] = 0.05685627;
        weights[5] = 0.04244593;
        weights[6] = 0.02979672;
        weights[7] = 0.01966869;
        weights[8] = 0.01220834;
        weights[9] = 0.00712544;
        weights[10] = 0.00391057;
        weights[11] = 0.00201809;
        weights[12] = 0.00097930;

        offsets[0] = 0.66494846;
        offsets[1] = 2.49034905;
        offsets[2] = 4.48263311;
        offsets[3] = 6.47492552;
        offsets[4] = 8.46722984;
        offsets[5] = 10.45954895;
        offsets[6] = 12.45188808;
        offsets[7] = 14.44425011;
        offsets[8] = 16.43663788;
        offsets[9] = 18.42905617;
        offsets[10] = 20.42150688;
        offsets[11] = 22.41399384;
        offsets[12] = 24.40652084;
    }
    else if (radius <= 27) {
        steps = 14;
        weights[0] = 0.06871720;
        weights[1] = 0.08815804;
        weights[2] = 0.08036669;
        weights[3] = 0.06949072;
        weights[4] = 0.05699201;
        weights[5] = 0.04433407;
        weights[6] = 0.03271127;
        weights[7] = 0.02289252;
        weights[8] = 0.01519587;
        weights[9] = 0.00956738;
        weights[10] = 0.00571342;
        weights[11] = 0.00323620;
        weights[12] = 0.00173864;
        weights[13] = 0.00088596;

        offsets[0] = 0.66519141;
        offsets[1] = 2.49171162;
        offsets[2] = 4.48508406;
        offsets[3] = 6.47846127;
        offsets[4] = 8.47184658;
        offsets[5] = 10.46524143;
        offsets[6] = 12.45864868;
        offsets[7] = 14.45207024;
        offsets[8] = 16.44550896;
        offsets[9] = 18.43896484;
        offsets[10] = 20.43244362;
        offsets[11] = 22.42594528;
        offsets[12] = 24.41947365;
        offsets[13] = 26.41302872;
    }
    else if (radius <= 29) {
        steps = 15;
        weights[0] = 0.06396806;
        weights[1] = 0.08249077;
        weights[2] = 0.07613957;
        weights[3] = 0.06713246;
        weights[4] = 0.05654208;
        weights[5] = 0.04549127;
        weights[6] = 0.03496240;
        weights[7] = 0.02566796;
        weights[8] = 0.01800106;
        weights[9] = 0.01205929;
        weights[10] = 0.00771723;
        weights[11] = 0.00471757;
        weights[12] = 0.00275480;
        weights[13] = 0.00153666;
        weights[14] = 0.00081881;

        offsets[0] = 0.66539001;
        offsets[1] = 2.49282646;
        offsets[2] = 4.48708963;
        offsets[3] = 6.48135614;
        offsets[4] = 8.47562790;
        offsets[5] = 10.46990585;
        offsets[6] = 12.46419144;
        offsets[7] = 14.45848656;
        offsets[8] = 16.45279312;
        offsets[9] = 18.44711113;
        offsets[10] = 20.44144249;
        offsets[11] = 22.43579102;
        offsets[12] = 24.43015480;
        offsets[13] = 26.42453575;
        offsets[14] = 28.41893768;
    }
    else if (radius <= 31) {
        steps = 16;
        weights[0] = 0.05991369;
        weights[1] = 0.07758097;
        weights[2] = 0.07231852;
        weights[3] = 0.06476077;
        weights[4] = 0.05571122;
        weights[5] = 0.04604065;
        weights[6] = 0.03655175;
        weights[7] = 0.02787681;
        weights[8] = 0.02042424;
        weights[9] = 0.01437529;
        weights[10] = 0.00971975;
        weights[11] = 0.00631337;
        weights[12] = 0.00393945;
        weights[13] = 0.00236144;
        weights[14] = 0.00135983;
        weights[15] = 0.00075225;

        offsets[0] = 0.66554797;
        offsets[1] = 2.49371290;
        offsets[2] = 4.48868465;
        offsets[3] = 6.48365831;
        offsets[4] = 8.47863579;
        offsets[5] = 10.47361755;
        offsets[6] = 12.46860409;
        offsets[7] = 14.46359730;
        offsets[8] = 16.45859718;
        offsets[9] = 18.45360756;
        offsets[10] = 20.44862556;
        offsets[11] = 22.44365311;
        offsets[12] = 24.43869400;
        offsets[13] = 26.43374634;
        offsets[14] = 28.42881012;
        offsets[15] = 30.42388916;
    }
    else if (radius <= 33) {
        steps = 17;
        weights[0] = 0.05626789;
        weights[1] = 0.07311317;
        weights[2] = 0.06872338;
        weights[3] = 0.06235151;
        weights[4] = 0.05460383;
        weights[5] = 0.04615650;
        weights[6] = 0.03765964;
        weights[7] = 0.02965876;
        weights[8] = 0.02254568;
        weights[9] = 0.01654273;
        weights[10] = 0.01171613;
        weights[11] = 0.00800931;
        weights[12] = 0.00528492;
        weights[13] = 0.00336601;
        weights[14] = 0.00206931;
        weights[15] = 0.00122792;
        weights[16] = 0.00070331;

        offsets[0] = 0.66568094;
        offsets[1] = 2.49445939;
        offsets[2] = 4.49002790;
        offsets[3] = 6.48559809;
        offsets[4] = 8.48117065;
        offsets[5] = 10.47674561;
        offsets[6] = 12.47232437;
        offsets[7] = 14.46790791;
        offsets[8] = 16.46349525;
        offsets[9] = 18.45908928;
        offsets[10] = 20.45469093;
        offsets[11] = 22.45029831;
        offsets[12] = 24.44591331;
        offsets[13] = 26.44153595;
        offsets[14] = 28.43717003;
        offsets[15] = 30.43281174;
        offsets[16] = 32.42846298;
    }
    else if (radius <= 35) {
        steps = 18;
        weights[0] = 0.05310436;
        weights[1] = 0.06919800;
        weights[2] = 0.06548640;
        weights[3] = 0.06005197;
        weights[4] = 0.05336076;
        weights[5] = 0.04594468;
        weights[6] = 0.03833250;
        weights[7] = 0.03098971;
        weights[8] = 0.02427652;
        weights[9] = 0.01842781;
        weights[10] = 0.01355438;
        weights[11] = 0.00966059;
        weights[12] = 0.00667185;
        weights[13] = 0.00446486;
        weights[14] = 0.00289525;
        weights[15] = 0.00181922;
        weights[16] = 0.00110764;
        weights[17] = 0.00065348;

        offsets[0] = 0.66578931;
        offsets[1] = 2.49506807;
        offsets[2] = 4.49112320;
        offsets[3] = 6.48717976;
        offsets[4] = 8.48323727;
        offsets[5] = 10.47929764;
        offsets[6] = 12.47535992;
        offsets[7] = 14.47142506;
        offsets[8] = 16.46749496;
        offsets[9] = 18.46356773;
        offsets[10] = 20.45964622;
        offsets[11] = 22.45572853;
        offsets[12] = 24.45181656;
        offsets[13] = 26.44791031;
        offsets[14] = 28.44401169;
        offsets[15] = 30.44011879;
        offsets[16] = 32.43623352;
        offsets[17] = 34.43235397;
    }
    else {
        steps = 19;
        weights[0] = 0.05021843;
        weights[1] = 0.06559712;
        weights[2] = 0.06244280;
        weights[3] = 0.05778965;
        weights[4] = 0.05199815;
        weights[5] = 0.04548788;
        weights[6] = 0.03868776;
        weights[7] = 0.03199053;
        weights[8] = 0.02571813;
        weights[9] = 0.02010145;
        weights[10] = 0.01527514;
        weights[11] = 0.01128530;
        weights[12] = 0.00810608;
        weights[13] = 0.00566081;
        weights[14] = 0.00384341;
        weights[15] = 0.00253702;
        weights[16] = 0.00162818;
        weights[17] = 0.00101589;
        weights[18] = 0.00061626;

        offsets[0] = 0.66588259;
        offsets[1] = 2.49559236;
        offsets[2] = 4.49206638;
        offsets[3] = 6.48854160;
        offsets[4] = 8.48501778;
        offsets[5] = 10.48149586;
        offsets[6] = 12.47797489;
        offsets[7] = 14.47445679;
        offsets[8] = 16.47094154;
        offsets[9] = 18.46742821;
        offsets[10] = 20.46391869;
        offsets[11] = 22.46041298;
        offsets[12] = 24.45691109;
        offsets[13] = 26.45341301;
        offsets[14] = 28.44991875;
        offsets[15] = 30.44643021;
        offsets[16] = 32.44294739;
        offsets[17] = 34.43946838;
        offsets[18] = 36.43599701;
    }

    vec4 color = vec4(0);

    uint colorspace = get_layer_colorspace(layerIdx);

    for (int i = 0; i < steps; i++) {
        vec2 posOffset;
        if (vertical)
            posOffset = vec2(0, offsets[i]);
        else
            posOffset = vec2(offsets[i], 0);

        vec4 tmp0 = textureCond(layerSampler, layerIdx, pos - posOffset, unnormalized);
        tmp0.rgb = colorspace_plane_degamma_tf(tmp0.rgb, colorspace) * weights[i];
        color += tmp0;

        vec4 tmp1 = textureCond(layerSampler, layerIdx, pos + posOffset, unnormalized);
        tmp1.rgb = colorspace_plane_degamma_tf(tmp1.rgb, colorspace) * weights[i];
        color += tmp1;
    }

    if (vertical)
    {
        color.rgb = apply_layer_color_mgmt(color.rgb, colorspace);
    }

    return color;
}
