package android.util;

/** Android 3.0's JSON token kinds, in the platform's declaration order. */
public enum JsonToken {
  BEGIN_ARRAY,
  END_ARRAY,
  BEGIN_OBJECT,
  END_OBJECT,
  NAME,
  STRING,
  NUMBER,
  BOOLEAN,
  NULL,
  END_DOCUMENT
}
