package android.graphics;

// AOSP graphics/java/android/graphics/Shader.java: the base of the paint shaders,
// holding the local matrix. Canvas does not sample shaders.
public class Shader {
  public enum TileMode { CLAMP, REPEAT, MIRROR, DECAL }
  private Matrix localMatrix;
  public Shader() {}
  public boolean getLocalMatrix(Matrix out) {
    if (localMatrix == null || localMatrix.isIdentity()) return false;
    out.set(localMatrix);
    return true;
  }
  public void setLocalMatrix(Matrix matrix) {
    localMatrix = matrix == null || matrix.isIdentity() ? null : new Matrix(matrix);
  }
}
