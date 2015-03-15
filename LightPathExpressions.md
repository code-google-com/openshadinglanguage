_This document is a work in progress. Apologies for the rough state, but we thought it better to immediately give people a better feel for the light path expressions._

_We're still refining the notation, it may change a bit because although it's extremely powerful, we're also finding that it's a little tricky to deal with.  (It's this uncertainty that is why it's not yet enshrined in the language specification.)_

_Also, this page uses some notation peculiar to Arnold, SPI's in-house renderer.  As we edit this document, we will gradually transform it into some kind of more generic notation._

_Please, help us improve this document by reporting any problems, confusion sections, or questions to osl-dev@googlegroups.com._


---


# OSL Light Path Expressions #

It is common practice in production to have a renderer produce several simultaneous outputs.  In addition to the main "beauty" render that contains the full illumination, there may be a desire for separate images that consist of only the diffuse illumination, specular illumination, reflections, just the contribution of a subset of lights, and so on.  Such outputs are often recombined in various ways in subsequent compositing.

In many renderers, it is common practice for shaders to define many output parameters, one for each of these "arbitrary output variables" ("AOV's"), and then for the renderer to pair the AOV names to image output names.  The problem with this approach is that all the shaders in the scene must explicitly specify each AOV parameter (and to put the correctly-computed lighting component into it).  This tends to make the shaders very cluttered as the number of AOVs grow, and it's a lot of work to modify all shaders when novel AOVs are requested.

OSL-based renderers discourage the practice cluttering shaders with these outputs, and instead allow a renderer-side specification of which light paths contribute to which renderer outputs.  New outputs may be specified using _light path expressions_, which uses notation similar to standard regular expressions, without any modification to the shaders at all.  This is possible because OSL shaders are not computing raw colors, but rather are computing radiance closures, and the closure primitives (such as diffuse, phong, etc.) know what kind of light paths they are computing.


## Regular expressions ##

The regular expressions we introduce here filter light contributions in the scene and direct them to selected AOV. If you have no previous experience with what a regular expression is, we recommend to have a look [here](http://en.wikipedia.org/wiki/Regular_expression). Our regular expressions for light paths are almost like that but with a few extensions due to the specific problem we are dealing with. But to get started, you don't have to worry too much about those extensions.

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/original.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/original.jpg)

Above you see a full render produced by the almost universal regexp:

`"C[SGs]*D*<Ts>*[LO]"`

Which allows for everything but caustics. We will learn shortly what it means.

## Light language ##

If you are already familiar with regexps, first we have to introduce you to the concrete language that we have for light paths. As you know, in raytracing, what we call a light path is a sequence of light "bounces" or "scatterings" that in standard backward raytracing start at the camera and end at a light or an emitting object. We are following the general convention that our paths start from the camera even though we might internally reverse everything eventually. But the user won't notice that. A typical light path could have the following steps:

  1. Leave the camera, ray goes to the scene
  1. Hit a specular surface, ray gets reflected back to the scene
  1. Hit a diffuse surface, ray gets reflected back to the scene
  1. Hit a light source, end of the path

So in this path light goes from the reached light to the camera through 2 bounces. Each of these steps in the path is what we call an event. And a path is just a sequence of these events. They always start at the camera and end at a light or some special "source" object we might eventually use. But for the purpose of this introduction, let's assume they always finish with a light. We have single char aliases for the events. Just as an advance, this is a possible printout of the path above:


`CSDL`

Which stands for Camera, Specular, Diffuse and Light.

## Events ##

### Event type ###

An event has several features that define it. One is its type. The event type tells us what kind of light stop or transition is it about. It might be leaving the camera, bouncing on a surface or hitting a light. The current set of event types we have defined so far are these:

  * C: Camera
  * R: Surface Reflection
  * T: Surface Transmission
  * V: Volume
  * L: Light
  * O: Emitting object
  * B: Background

The letter at the beginning of the definition is the alias we are going to use when writing our expressions. For the moment just ignore V and B. There is an important to note difference between L and O. The first appears when we hit a shape that has been tagged as a light source. That means that we are going to sample directly from it for better quality. These light sources are currently limited to spheres, discs and quads. But you can also assign an emission value to a regular object of any type. That's going to throw light to the scene too, but paths ending there will finish with O instead of L. This way you can make a distintion between the two kinds of contributions without using custom labels. Also be aware that the current SSS implementation based on point clouds behaves as an emitter too. Therefore, it will be rendered under the O label too.

### Event scattering type ###

Only when it applies, a scattering label will be defined. We have narrowed down this classification to three cases plus a special label we use for convenience in transparent shadows.

  * D: Diffuse
  * G: Glossy
  * S: Singular, some people like it to be called reflection and refraction
  * s: Straight (special case of S, note the lower case)

The first three are just what you are used to. They speak for themselves. Singular means a perfect sharp reflection or refraction. Not blurred like in the Glossy case, which is like a middle ground between diffuse and singular. The special labels is `"s"` (lower case) or _straight_. It means that the ray passes through the surface untouched. It is like a perfect refraction but without changing the ray direction. We tagged it different so we can perform transparent sadows without relying on caustics.

### Event full description ###

Ok, we have seen the two different label types that an event in the path can have. Now, how do you fully describe an event? Easy, just by providing these two basic labels. But we know the second one does not always apply (scattering). In that case we are going to write down a `"x"` char to denote it is undefined. So going back to our original path:

  1. Leave the camera, ray goes to the scene
  1. Hit a specular surface, ray gets reflected back to the scene
  1. Hit a diffuse surface, ray gets reflected back to the scene
  1. Hit a light source, end of the path

We can now write it like this:

  1. `< C x >` (Camera)
  1. `< R S >` (Reflected, Singular)
  1. `< R D >` (Reflected, Diffuse)
  1. `< L x >` (Light)

So we can just put one after another and fully describe the path with the sequence:

`<Cx><RS><RD><Lx>`

Which in our previously less specific language was:

`CSDL`

But don't worry because in regular expressions you'll be using the short version most of the time.

### Regular expressions on light ###

Please, remember that `"."` (dot) in regexps is the wildcard. That is a symbol that matches anything but just one time. And `"*"` is a modifier on the previous expression that allows it to repeat any number of times. So the expression `"."` will match any event just one time, but `".*"` will match any repetitions of any event, including zero repetitions.

The symbols in our language are not characters like in standard regexps, but events. And the notation we have for events is `<ab>` with the two labels. Let's say you want to match any path that starts in the camera and has only diffuse light exchanges (including translucent). An example path of this case would be:

`<Cx><RD><TD><RD><Lx>`

To match a path like this you could write this expression:

`<Cx><.D>*<Lx>`

So `"< . D > *"` will match any repetition of surfaces reflecting or transmitting (note the wildcard) in a diffuse way. But this can be simplfied a lot. We know that everything acting diffuse in our scenes is going to be a surface reflection or transmission. And we also know that for a camera and a light, the other label is going to be undefined, so we can actually write:


`<C.><.D>*<L.>`

And here comes the syntax trick. Our parser is smart enough to know that if you say just `"D"` or `"L"` you are actually meaning `"< . D >"` and `"< L . >"`. Yes, it knows the right position for every label, so you can just write:


`CD*L`

And it would do the same (see image).

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/diffuse.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/diffuse.jpg)

You save typing time and the expression is more clear to an eventual reader. So you might be wondering: why have that previous complexity for defining patterns? Why not always use this last form? Well, if these simplified expressions are enough for your render, good for you, but you may find a different situation. for instance, let's say you want to capture all the diffuse bounces but you don't care about translucent contributions. In other words, you just want the matte reflection as if everything was made out of chalk. In that case you want to use this expression:


`C<RD>*L`

So as you see we use the simplified form for C and L but we open the complexity for the diffuse bounce and we specify that it has to be a Reflective surface and Diffuse. Just to give you another example, say you want the same thing but only seen through perfect reflections and refractions:


`CS+<RD>*L`

For those of you not familiar with regular expressions, `"+"` means one or more repetitions. So that pattern will only show things that are reflected or refracted at least once. This would be the resulting image:

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/sdiffuse.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/sdiffuse.jpg)

Notice the sphere on the right side doesn't appear because that is classified as glossy (G). We can make it appear if we replace or `"S"` by `"[SG]"` so both are allowed. Again, if you just want the refractions for instance, you expand the expression to this:


`C<TS>+<RD>*L`

And suddenly the reflection of the lamp and others disappear ...

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/stdiffuse.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/stdiffuse.jpg)

### Mapping regexps to AOV's ###

Now you can map any valid regular expression to any AOV that you have defined. So you may issue renderer commands like this:

```
    Output ("beauty.tif", "rgb", "C[SGs]*D*<Ts>*L");
    Output ("diffuse.tif", "rgb", "CD+L");
```

N.B.: For illustration, we are assuming a renderer command Output(filename, format, data-regex...), and that any number of these commands may be issued to give multiple outputs.  We will continue to use this notation in the remaining examples.

Let's extend our notation so that multiple regexps can be directed to the same output, like this:

```
    Output ("diffuse.tif", "rgb", "CDDL", "CDL");
```

And in this case we are accumulating in diffuse only direct lighting and one bounce diffuse indirect lighting. This one is actually equivalent to this single regexp setup:

```
    Output ("diffuse.tif", "rgb", "CD{1,2}L");
```


### Using regexps to produce an alpha channel ###

There is no specific alpha output in the new OSL integrator, but you can easily produce one. And actually the default rules set it up for you, but let's see how it works. You had seen in the label listing that we have something called B (Background). This is a special source of "light", but don't get confused, it is not the fully shaded background (which is presented as L, a normal light). This is a special source that always give 1.0 values, so we can compute an image of the visibility of the background. and it will be independent of what it is actually showing up behind objects. Take a look at this picture:

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/aoriginal.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/aoriginal.jpg)

So let's say you want to get the visibility of the background. No need for color so we setup a FLOAT AOV and map this regular expression to it:

```
    Output ("vis.tif", "float", "C<Ts>*B");
```

Note the expression, we are saying here that we want anything that goes transmitted in a straight direction (transparency) and touches the background. You could add standard refractions here by adding the S label ored with s. But this will be the most typical setup. This gives us this image:

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/avisibility.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/avisibility.jpg)

But the alpha channel we want is acually the negative of this one. You could do that in postproduction, but we provide a way to modify a rule so it outputs the negative by prepending the expression with the exclamation mark `"!"`. So if you try this expression:

```
    Output ("vis.tif", "float", "!C<Ts>*B");
```


The image you will get is this one:

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/aalpha.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/aalpha.jpg)

Good, you got your alpha channel as a separate AOV, but what if you want to put it in the actual alpha channel of an RGBA AOV? You can do that by appending your expression to the standard rgb expression for an AOV. Look at this example:

```
    Output ("beauty.tif", "rgba", "C[SGs]*D*<Ts>*L !C<Ts>*B");
```

We first write a "beauty" expression for the color component and then another one that will be mapped to the alpha channel and voila!

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/afinal.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/afinal.jpg)

You got it. Note that this dual regexp setup is only valid for RGBA AOV's.

## Custom labels ##

Using this basic set of labels is good for managing standard image components. But might need to isolate specific objects or light in the scene. For this purpose OSL provides the capability of tagging with arbitrary labels the closures you return from your shader. The syntax looks like this:

```
  Ci = Kd * diffuse(N, "label", "alice") + Ks * phong(N, exp, "label", "alice");
```

Remember that closures define the way the surface behaves. Light might pass through the phong closure, and because phong is a glossy reflection, that event will get the G label in the light path. But in addition we are defining a custom label with the pair `"label", "alice"`, so the light passing there will also get "alice" as a label. So the event that used to be formed this way:


`<RG>`

will now appear as ...


`<RG'alice'>`

But don't hesitate about your existing regular expressions cause the expression `"<RG>*"` will still match that event. This happens because when you define an event with `"G"` or `"<RG>"` there is an implicit `".*"` at the end. So internally it's something like `""`. You don't have to worry about matching extra custom labels, the engine will take care of it for you. So what if you want to match only your tagged phong? Well, as soon as your regular expression specifies a custom label:


`<RG'alice'>`

It will only match `"<RG>"` events which in addition define the label `"alice"`. With a bit of standard regular expression magic you can also match anything of that type except those tagged with `"alice"`:


`<RG[^'alice']>`

See the regular expressions references linked at the top of this document if you are new to regular expressions. So as you see custom labels will always be enclosed in single quotes, and it is the shader that assigns them to surfaces and lights by the closure arguments. With this and the power of regexps you can do any kind of matching of scene interactions. Let's say you want only the light reflected in the object tagged as alice from the light `"light1"`:


`C'alice'<L.'light1'>`

Remember that when we write just `'alice'` it actually expands internally to `"<..'alice'>"`. So it will match diffuse, specular, glossy, everything as long as it is tagged as "alice". But it might be the case that we don't use the label "light1" for any other things but lights, so why waste screen ink with redundant information? Let's just write:


`C'alice''light1'`

Let's go a bit further. Now we want all the light in alice being reflected by objects tagged as "wall". Try this one:


`C'alice''wall'+L`

And so on. But let's look at an example image. We have this good old cornell box:

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/cb_original.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/cb_original.jpg)

Where we are using the same shader for all the objects and changing the parameters when creating instances in the ass file. So the definition of the shader is pretty simple:

```
surface matte (float Kd = 1, color Cs = 1, string label = "foo")
{
   Ci = Kd * Cs * diffuse (N, "label", label);
}
```

The only new thing is that we have a label parameter that is going to be passed directly to the closure. That's assigning the tag to the diffuse term, which is the only one here. That allows us to create the shader in the ass file like this:

```
    Parameter ("Cs", color(0.1, 1, 0.1));
    Parameter ("label", "greenwall");
    Shader ("surface", "matte", "green_layer");
```

So we give the label "greenwall". This goes obviously to the green wall of the image. We can also do the same for the light shader:

```
surface emitter (float Ke = 1, color Cs = 1, string label = "foo")
{
    Ci = Ke * Cs * emission("label", label);
}
```

And then instantiate two light shaders for or two quad emitters on the ceiling like this:

```
    Parameter ("Ke", 100.0);
    Parameter ("label", "light1");
    Shader ("surface", "emitter");

    ...

    Parameter ("Ke", 100.0);
    Parameter ("label", "light2");
    Shader ("surface", "emitter");
```


With light1 being the one in the left side and light2 on the right side. So now we are ready to test this custom label filtering with regular expressions. We could ask for all the light being reflected by the greenwall once and then bounced on any other surface. This would be the regexp:

```
    Output ("beauty.tif", "rgb", "CD*'greenwall'L");
```


And this is the image you get:

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/cb_allgreen.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/cb_allgreen.jpg)

But maybe you just want the light directly reflected by the green wall (no inter-object light), and say you just need the light2 for it (the one which lays to the right side). So you won't get as much energy as in the original. This would be the setup:

```
    Output ("beauty.tif", "rgb", "C'greenwall''light2'");
```

And this would be the result:

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/cb_1green.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/cb_1green.jpg)

Or we could try to get all the light in diffuse surfaces being reflected by the red wall, but not that directly reflected by it (so the red wall won't show up). This is the setup for that, note the `"[^'redwall']+"` that forces at least one reflector which is not tagged as "redwall":

```
    Output ("beauty.tif", "rgb", "C[^'redwall']+'redwall'L");
```

And this is what you get:

![http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/cb_red.jpg](http://openshadinglanguage.googlecode.com/svn/trunk/src/doc/Figures/LightPathExpr/cb_red.jpg)

Assuming of course, that we have tagged the red wall shader with "redwall" just the same way we did with the green one.