import java.nio.file.*;
import java.lang.reflect.*;
import java.util.regex.*;
public class ManifestApiFixture {
  public static void main(String[] args) throws Exception {
    Class<?> root = Class.forName("android.Manifest");
    if (!Modifier.isPublic(root.getModifiers()) || !Modifier.isFinal(root.getModifiers())) throw new AssertionError("Root API modifiers");
    root.getConstructor().newInstance();
    String api = Files.readString(Path.of(args[0]));
    Pattern blocks = Pattern.compile("public static final class Manifest\\.(permission(?:_group)?) \\{(.*?)\\n  \\}", Pattern.DOTALL);
    Matcher b = blocks.matcher(api); int classes=0, fields=0;
    while (b.find()) {
      Class<?> type = Class.forName("android.Manifest$"+b.group(1));
      if (!Modifier.isPublic(type.getModifiers()) || !Modifier.isStatic(type.getModifiers()) || !Modifier.isFinal(type.getModifiers())) throw new AssertionError("Nested API modifiers");
      type.getConstructor().newInstance(); classes++;
      Matcher f = Pattern.compile("field (?:@Deprecated )?public static final String ([A-Z0-9_]+) = \"([^\"]+)\";").matcher(b.group(2));
      int count=0;
      while(f.find()) {
        Field field=type.getField(f.group(1));
        if(field.getType()!=String.class || !Modifier.isPublic(field.getModifiers()) || !Modifier.isStatic(field.getModifiers()) || !Modifier.isFinal(field.getModifiers()) || !f.group(2).equals(field.get(null))) throw new AssertionError("Public API field mismatch");
        fields++; count++;
      }
      // API 32 is the required baseline; newer SDK constants may also be exposed.
      if (count == 0) throw new AssertionError("Empty public API class");
    }
    if(classes!=2 || fields<100) throw new AssertionError("Incomplete public API fixture");
    for (String name : new String[]{"POST_NOTIFICATIONS", "READ_MEDIA_AUDIO", "READ_MEDIA_IMAGES", "READ_MEDIA_VIDEO"}) {
      Object value = Class.forName("android.Manifest$permission").getField(name).get(null);
      if (!("android.permission." + name).equals(value)) throw new AssertionError("Newer API constant lost: " + name);
    }
    System.out.println("PASS: public Manifest classes="+(classes+1)+" fields="+fields);
  }
}
