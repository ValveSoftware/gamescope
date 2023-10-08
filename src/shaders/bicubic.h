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
outAU4 con2,
// This are the bicubic configurable parameters
AU1 B,
AU1 C,
// This the rendered image resolution
AF1 inputImageResolutionX,
AF1 inputImageResolutionY,
// This is the resolution of the resource containing the input image (useful for dynamic resolution)
AF1 inputFrameX,
AF1 inputFrameY,
// This is the window width / height
AF1 outputSizeInPixelsX,
AF1 outputSizeInPixelsY)
{
 // Input/Output size information
 con0[0]=AU1_AF1(inputImageResolutionX*ARcpF1(inputFrameX));
 con0[1]=AU1_AF1(inputImageResolutionY*ARcpF1(inputFrameY));
 con0[2]=AU1_AF1(inputFrameX);
 con0[3]=AU1_AF1(inputFrameY);

 // Viewport pixel position to normalized image space.
 con1[0]=AU1_AF1(ARcpF1(inputImageResolutionX));
 con1[1]=AU1_AF1(ARcpF1(inputImageResolutionY));
 con1[2]=AU1_AF1(AF1_( 1.0)*ARcpF1(inputImageResolutionX));
 con1[3]=AU1_AF1(AF1_(-1.0)*ARcpF1(inputImageResolutionY));

 // Bicubic parameters
 con2[0]=AU1_AF1(B);
 con2[1]=AU1_AF1(C);
 con2[2]=AU1_AF1(AF1_(0.5)*inputImageResolutionX*ARcpF1(inputFrameX)-AF1_(0.5));
 con2[3]=AU1_AF1(AF1_(0.5)*inputImageResolutionY*ARcpF1(inputFrameY)-AF1_(0.5));
}
