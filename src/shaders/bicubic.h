//_____________________________________________________________/\_______________________________________________________________
//==============================================================================================================================
//
//
//                    BICUBIC IMAGE SCALING
//
//
//------------------------------------------------------------------------------------------------------------------------------
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//_____________________________________________________________/\_______________________________________________________________
//==============================================================================================================================
//                                                      CONSTANT SETUP
//==============================================================================================================================
// Call to setup required constant values (works on CPU or GPU).
A_STATIC void BicubicCon(
outAU4 con0,
outAU4 con1,
// Configurable parameters
AU1 B,
AU1 C,
// This the rendered image resolution
AF1 inputRenderedSizeX,
AF1 inputRenderedSizeY,
// This is the resolution of the resource containing the input image (useful for dynamic resolution)
AF1 inputCurrentSizeX,
AF1 inputCurrentSizeY,
// This is the window width / height
AF1 outputTargetSizeX,
AF1 outputTargetSizeY)
{
 // Input/Output size information
 con0[0]=AU1_AF1(inputRenderedSizeX);
 con0[1]=AU1_AF1(inputRenderedSizeY);
 con0[2]=AU1_AF1(inputCurrentSizeX);
 con0[3]=AU1_AF1(inputCurrentSizeY);

 // Viewport pixel position to normalized image space.
 con1[0]=AU1_AF1(outputTargetSizeX);
 con1[1]=AU1_AF1(outputTargetSizeY);
 con1[2]=B;
 con1[3]=C;
}
